// kv_record_store_capped.cpp

/**
*    Copyright (C) 2014 MongoDB Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/storage/kv/dictionary/kv_record_store_capped.h"
#include "mongo/db/storage/kv/dictionary/visible_id_tracker.h"
#include "mongo/db/storage/oplog_hack.h"

namespace mongo {

    KVRecordStoreCapped::KVRecordStoreCapped( KVDictionary *db,
                                              OperationContext* opCtx,
                                              const StringData& ns,
                                              const StringData& ident,
                                              const CollectionOptions& options,
                                              KVSizeStorer *sizeStorer)
        : KVRecordStore(db, opCtx, ns, ident, options, sizeStorer),
          _cappedMaxSize(options.cappedSize ? options.cappedSize : 4096 ),
          _cappedMaxDocs(options.cappedMaxDocs ? options.cappedMaxDocs : -1),
          _cappedDeleteCallback(NULL),
          _isOplog(NamespaceString::oplog(ns)),
          _idTracker(_isOplog ? new OplogIdTracker() : new CappedIdTracker())
    {}

    bool KVRecordStoreCapped::needsDelete(OperationContext* txn) const {
        if (dataSize(txn) > _cappedMaxSize) {
            // .. too many bytes
            return true;
        }

        if ((_cappedMaxDocs != -1) && (numRecords(txn) > _cappedMaxDocs)) {
            // .. too many documents
            return true;
        }

        // we're ok
        return false;
    }

    void KVRecordStoreCapped::deleteAsNeeded(OperationContext *txn) {
        if (!needsDelete(txn)) {
            // nothing to do
            return;
        }

        // Only one thread should do deletes at a time, otherwise they'll conflict.
        boost::mutex::scoped_lock lock(_cappedDeleteMutex, boost::try_to_lock);
        if (!lock) {
            return;
        }

        // Delete documents while we are over-full and the iterator has more.
        for (boost::scoped_ptr<RecordIterator> iter(getIterator(txn));
             needsDelete(txn) && !iter->isEOF(); ) {
            const RecordId oldest = iter->getNext();
            deleteRecord(txn, oldest);
        }
    }

    StatusWith<RecordId> KVRecordStoreCapped::insertRecord( OperationContext* txn,
                                                           const char* data,
                                                           int len,
                                                           bool enforceQuota ) {
        if (len > _cappedMaxSize) {
            // this single document won't fit
            return StatusWith<RecordId>(ErrorCodes::BadValue,
                                       "object to insert exceeds cappedMaxSize");
        }

        StatusWith<RecordId> id(Status::OK());
        if (_isOplog) {
            id = oploghack::extractKey(data, len);
            if (!id.isOK()) {
                return id;
            }

            Status s = _insertRecord(txn, id.getValue(), Slice(data, len));
            if (!s.isOK()) {
                return StatusWith<RecordId>(s);
            }
        } else {
            // insert using the regular KVRecordStore insert implementation..
            id = KVRecordStore::insertRecord(txn, data, len, enforceQuota);
            if (!id.isOK()) {
                return id;
            }
        }

        _idTracker->addUncommittedId(txn, id.getValue());

        // ..then delete old data as needed
        deleteAsNeeded(txn);

        return id;
    }

    StatusWith<RecordId> KVRecordStoreCapped::insertRecord( OperationContext* txn,
                                                           const DocWriter* doc,
                                                           bool enforceQuota ) {
        // We need to override every insertRecord overload, otherwise the compiler gets mad.
        Slice value(doc->documentSize());
        doc->writeDocument(value.mutableData());
        return insertRecord(txn, value.data(), value.size(), enforceQuota);
    }

    void KVRecordStoreCapped::deleteRecord( OperationContext* txn, const RecordId& dl ) {
        if (_cappedDeleteCallback) {
            // need to notify higher layers that a RecordId is about to be deleted
            uassertStatusOK(_cappedDeleteCallback->aboutToDeleteCapped(txn, dl));
        }
        KVRecordStore::deleteRecord(txn, dl);
    }

    void KVRecordStoreCapped::appendCustomStats( OperationContext* txn,
                                                 BSONObjBuilder* result,
                                                 double scale ) const {
        result->append("capped", true);
        result->appendIntOrLL("max", _cappedMaxDocs);
        result->appendIntOrLL("maxSize", _cappedMaxSize);
        KVRecordStore::appendCustomStats(txn, result, scale);
    }

    void KVRecordStoreCapped::temp_cappedTruncateAfter(OperationContext* txn,
                                                       RecordId end,
                                                       bool inclusive) {
        // Not very efficient, but it should only be used by tests.
        for (boost::scoped_ptr<RecordIterator> iter(
                 getIterator(txn, end, CollectionScanParams::FORWARD));
             !iter->isEOF(); ) {
            RecordId loc = iter->getNext();
            if (!inclusive && loc == end) {
                continue;
            }
            WriteUnitOfWork wu( txn );
            deleteRecord(txn, loc);
            wu.commit();
        }
    }

    RecordId KVRecordStoreCapped::oplogStartHack(OperationContext* txn,
                                                 const RecordId& startingPosition) const {
        if (!_idTracker) {
            return RecordId().setInvalid();
        }

        RecordId lowestInvisible = _idTracker->lowestInvisible();
        for (scoped_ptr<RecordIterator> iter(getIterator(txn, startingPosition, CollectionScanParams::BACKWARD));
             !iter->isEOF(); iter->getNext()) {
            if (iter->curr() <= startingPosition && iter->curr() < lowestInvisible) {
                return iter->curr();
            }
        }
        return RecordId().setInvalid();
    }

    Status KVRecordStoreCapped::oplogDiskLocRegister(OperationContext* txn,
                                                     const OpTime& opTime) {
        StatusWith<RecordId> loc = oploghack::keyForOptime( opTime );
        if ( !loc.isOK() )
            return loc.getStatus();

        _idTracker->addUncommittedId(txn, loc.getValue());
        return Status::OK();
    }

    RecordIterator* KVRecordStoreCapped::getIterator(OperationContext* txn,
                                                     const RecordId& start,
                                                     const CollectionScanParams::Direction& dir) const {
        auto_ptr<RecordIterator> iter(KVRecordStore::getIterator(txn, start, dir));
        if (dir == CollectionScanParams::FORWARD) {
            KVRecordIterator *kvIter = dynamic_cast<KVRecordIterator *>(iter.get());
            invariant(kvIter);
            _idTracker->setIteratorRestriction(kvIter);
        }
        return iter.release();
    }

} // namespace mongo

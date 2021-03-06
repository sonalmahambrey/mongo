/**
*    Copyright (C) 2014 MongoDB Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/db/storage/rocks/rocks_recovery_unit.h"

#include <rocksdb/comparator.h>
#include <rocksdb/db.h>
#include <rocksdb/iterator.h>
#include <rocksdb/slice.h>
#include <rocksdb/options.h>
#include <rocksdb/write_batch.h>
#include <rocksdb/utilities/write_batch_with_index.h>

#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/rocks/rocks_transaction.h"
#include "mongo/util/log.h"

namespace mongo {

    RocksRecoveryUnit::RocksRecoveryUnit(RocksTransactionEngine* transactionEngine, rocksdb::DB* db,
                                         bool durable)
        : _transactionEngine(transactionEngine),
          _db(db),
          _durable(durable),
          _transaction(transactionEngine),
          _writeBatch(),
          _snapshot(NULL),
          _depth(0) {}

    RocksRecoveryUnit::~RocksRecoveryUnit() {
        _abort();
    }

    void RocksRecoveryUnit::beginUnitOfWork() {
        _depth++;
    }

    void RocksRecoveryUnit::commitUnitOfWork() {
        if (_depth > 1) {
            return; // only outermost gets committed.
        }

        if (_writeBatch) {
            _commit();
        }

        for (Changes::const_iterator it = _changes.begin(), end = _changes.end(); it != end; ++it) {
            (*it)->commit();
        }
        _changes.clear();

        _releaseSnapshot();
    }

    void RocksRecoveryUnit::endUnitOfWork() {
        _depth--;
        if (_depth == 0) {
            _abort();
        }
    }

    bool RocksRecoveryUnit::awaitCommit() {
        // TODO
        return true;
    }

    void RocksRecoveryUnit::commitAndRestart() {
        invariant( _depth == 0 );
        commitUnitOfWork();
    }

    // lazily initialized because Recovery Units are sometimes initialized just for reading,
    // which does not require write batches
    rocksdb::WriteBatchWithIndex* RocksRecoveryUnit::writeBatch() {
        if (!_writeBatch) {
            // this assumes that default column family uses default comparator. change this if you
            // change default column family's comparator
            _writeBatch.reset(
                new rocksdb::WriteBatchWithIndex(rocksdb::BytewiseComparator(), 0, true));
        }

        return _writeBatch.get();
    }

    void RocksRecoveryUnit::setOplogReadTill(const RecordId& record) { _oplogReadTill = record; }

    void RocksRecoveryUnit::registerChange(Change* change) { _changes.push_back(change); }

    void RocksRecoveryUnit::_releaseSnapshot() {
        if (_snapshot) {
            _db->ReleaseSnapshot(_snapshot);
            _snapshot = nullptr;
        }
    }

    void RocksRecoveryUnit::_commit() {
        invariant(_writeBatch);
        for (auto pair : _deltaCounters) {
            auto& counter = pair.second;
            counter._value->fetch_add(counter._delta, std::memory_order::memory_order_relaxed);
            long long newValue = counter._value->load(std::memory_order::memory_order_relaxed);

            // TODO: make the encoding platform indepdent.
            const char* nr_ptr = reinterpret_cast<char*>(&newValue);
            writeBatch()->Put(pair.first, rocksdb::Slice(nr_ptr, sizeof(long long)));
        }

        if (_writeBatch->GetWriteBatch()->Count() != 0) {
            // Order of operations here is important. It needs to be synchronized with
            // _transaction.recordSnapshotId() and _db->GetSnapshot() and
            rocksdb::WriteOptions writeOptions;
            writeOptions.disableWAL = !_durable;
            auto status = _db->Write(rocksdb::WriteOptions(), _writeBatch->GetWriteBatch());
            if (!status.ok()) {
                log() << "uh oh: " << status.ToString();
                invariant(!"rocks write batch commit failed");
            }
            _transaction.commit();
        }
        _deltaCounters.clear();
        _writeBatch.reset();
    }

    void RocksRecoveryUnit::_abort() {
        for (Changes::const_reverse_iterator it = _changes.rbegin(), end = _changes.rend();
                it != end; ++it) {
            (*it)->rollback();
        }
        _changes.clear();

        _transaction.abort();
        _deltaCounters.clear();
        _writeBatch.reset();

        _releaseSnapshot();
    }

    const rocksdb::Snapshot* RocksRecoveryUnit::snapshot() {
        if ( !_snapshot ) {
            // Order of operations here is important. It needs to be synchronized with
            // _db->Write() and _transaction.commit()
            _transaction.recordSnapshotId();
            _snapshot = _db->GetSnapshot();
        }

        return _snapshot;
    }

    rocksdb::Status RocksRecoveryUnit::Get(rocksdb::ColumnFamilyHandle* columnFamily,
                                           const rocksdb::Slice& key, std::string* value) {
        if (_writeBatch && _writeBatch->GetWriteBatch()->Count() > 0) {
            boost::scoped_ptr<rocksdb::WBWIIterator> wb_iterator(
                _writeBatch->NewIterator(columnFamily));
            wb_iterator->Seek(key);
            if (wb_iterator->Valid() && wb_iterator->Entry().key == key) {
                const auto& entry = wb_iterator->Entry();
                if (entry.type == rocksdb::WriteType::kDeleteRecord) {
                    return rocksdb::Status::NotFound();
                }
                // TODO avoid double copy
                *value = std::string(entry.value.data(), entry.value.size());
                return rocksdb::Status::OK();
            }
        }
        rocksdb::ReadOptions options;
        options.snapshot = snapshot();
        return _db->Get(options, columnFamily, key, value);
    }

    rocksdb::Iterator* RocksRecoveryUnit::NewIterator(rocksdb::ColumnFamilyHandle* columnFamily) {
        invariant(columnFamily != _db->DefaultColumnFamily());

        rocksdb::ReadOptions options;
        options.snapshot = snapshot();
        auto iterator = _db->NewIterator(options, columnFamily);
        if (_writeBatch && _writeBatch->GetWriteBatch()->Count() > 0) {
            iterator = _writeBatch->NewIteratorWithBase(columnFamily, iterator);
        }
        return iterator;
    }

    void RocksRecoveryUnit::incrementCounter(const rocksdb::Slice& counterKey,
                                             std::atomic<long long>* counter, long long delta) {
        if (delta == 0) {
            return;
        }

        auto pair = _deltaCounters.find(counterKey.ToString());
        if (pair == _deltaCounters.end()) {
            _deltaCounters[counterKey.ToString()] =
                mongo::RocksRecoveryUnit::Counter(counter, delta);
        } else {
            pair->second._delta += delta;
        }
    }

    long long RocksRecoveryUnit::getDeltaCounter(const rocksdb::Slice& counterKey) {
        auto counter = _deltaCounters.find(counterKey.ToString());
        if (counter == _deltaCounters.end()) {
            return 0;
        } else {
            return counter->second._delta;
        }
    }

    RocksRecoveryUnit* RocksRecoveryUnit::getRocksRecoveryUnit(OperationContext* opCtx) {
        return dynamic_cast<RocksRecoveryUnit*>(opCtx->recoveryUnit());
    }

}

////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2016 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Kaveh Vahedipour
////////////////////////////////////////////////////////////////////////////////

#include "State.h"

#include <velocypack/Buffer.h>
#include <velocypack/Slice.h>
#include <velocypack/velocypack-aliases.h>

#include <chrono>
#include <iomanip>
#include <sstream>
#include <thread>

#include "Aql/Query.h"
#include "Basics/VelocyPackHelper.h"
#include "RestServer/QueryRegistryFeature.h"
#include "Utils/OperationOptions.h"
#include "Utils/OperationResult.h"
#include "Utils/SingleCollectionTransaction.h"
#include "Utils/StandaloneTransactionContext.h"
#include "VocBase/collection.h"
#include "VocBase/vocbase.h"

using namespace arangodb;
using namespace arangodb::application_features;
using namespace arangodb::consensus;
using namespace arangodb::velocypack;
using namespace arangodb::rest;

State::State(std::string const& endpoint)
    : _vocbase(nullptr),
      _endpoint(endpoint),
      _collectionsChecked(false),
      _collectionsLoaded(false) {
  std::shared_ptr<Buffer<uint8_t>> buf = std::make_shared<Buffer<uint8_t>>();
  VPackSlice value = arangodb::basics::VelocyPackHelper::EmptyObjectValue();
  buf->append(value.startAs<char const>(), value.byteSize());
  if (!_log.size()) {
    _log.push_back(log_t(arangodb::consensus::index_t(0), term_t(0), arangodb::consensus::id_t(0), buf));
  }
}

State::~State() {}

bool State::persist(arangodb::consensus::index_t index, term_t term, arangodb::consensus::id_t lid,
                    arangodb::velocypack::Slice const& entry) {
  Builder body;
  body.add(VPackValue(VPackValueType::Object));
  std::ostringstream i_str;
  i_str << std::setw(20) << std::setfill('0') << index;
  body.add("_key", Value(i_str.str()));
  body.add("term", Value(term));
  body.add("leader", Value((uint32_t)lid));
  body.add("request", entry);
  body.close();

  TRI_ASSERT(_vocbase != nullptr);
  auto transactionContext =
    std::make_shared<StandaloneTransactionContext>(_vocbase);
  SingleCollectionTransaction trx (
    transactionContext, "log", TRI_TRANSACTION_WRITE);
  
  int res = trx.begin();

  if (res != TRI_ERROR_NO_ERROR) {
    THROW_ARANGO_EXCEPTION(res);
  }
  
  OperationResult result = trx.insert("log", body.slice(), _options);
  res = trx.finish(result.code);

  return (res == TRI_ERROR_NO_ERROR);
}

//Leader
std::vector<arangodb::consensus::index_t> State::log (
  query_t const& query, std::vector<bool> const& appl, term_t term, arangodb::consensus::id_t lid) {

  std::vector<arangodb::consensus::index_t> idx(appl.size());
  std::vector<bool> good = appl;
  size_t j = 0;
  
  MUTEX_LOCKER(mutexLocker, _logLock);  // log entries must stay in order
  for (auto const& i : VPackArrayIterator(query->slice())) {
    if (good[j]) {
      std::shared_ptr<Buffer<uint8_t>> buf =
          std::make_shared<Buffer<uint8_t>>();
      buf->append((char const*)i[0].begin(), i[0].byteSize());
      idx[j] = _log.back().index + 1;
      _log.push_back(log_t(idx[j], term, lid, buf));  // log to RAM
      persist(idx[j], term, lid, i[0]);                  // log to disk
      ++j;
    }
  }
  return idx;
}

// Follower
bool State::log(query_t const& queries, term_t term, arangodb::consensus::id_t lid,
                arangodb::consensus::index_t prevLogIndex, term_t prevLogTerm) {  // TODO: Throw exc
  if (queries->slice().type() != VPackValueType::Array) {
    return false;
  }
  MUTEX_LOCKER(mutexLocker, _logLock);  // log entries must stay in order
  for (auto const& i : VPackArrayIterator(queries->slice())) {
    try {
      std::shared_ptr<Buffer<uint8_t>> buf =
          std::make_shared<Buffer<uint8_t>>();
      buf->append((char const*)i.get("query").begin(),
                  i.get("query").byteSize());
      _log.push_back(log_t(i.get("index").getUInt(), term, lid, buf));
      persist(i.get("index").getUInt(), term, lid, i.get("query")); // to disk
    } catch (std::exception const& e) {
      LOG(ERR) << e.what();
    }
  
  }
  return true;
}

// Get log entries from indices "start" to "end"
std::vector<log_t> State::get(arangodb::consensus::index_t start, arangodb::consensus::index_t end) const {
  std::vector<log_t> entries;
  MUTEX_LOCKER(mutexLocker, _logLock);
  if (end == (std::numeric_limits<uint64_t>::max)()) end = _log.size() - 1;
  for (size_t i = start; i <= end; ++i) {  
    entries.push_back(_log[i]);
  }
  return entries;
}

std::vector<VPackSlice> State::slices(arangodb::consensus::index_t start, arangodb::consensus::index_t end) const {
  std::vector<VPackSlice> slices;
  MUTEX_LOCKER(mutexLocker, _logLock);
  if (end == (std::numeric_limits<uint64_t>::max)()) end = _log.size() - 1;
  for (size_t i = start; i <= end; ++i) {  // TODO:: Check bounds
    slices.push_back(VPackSlice(_log[i].entry->data()));
  }
  return slices;
}

log_t const& State::operator[](arangodb::consensus::index_t index) const {
  MUTEX_LOCKER(mutexLocker, _logLock);
  return _log[index];
}

log_t const& State::lastLog() const {
  MUTEX_LOCKER(mutexLocker, _logLock);
  return _log.back();
}

bool State::setEndPoint(std::string const& endpoint) {
  _endpoint = endpoint;
  _collectionsChecked = false;
  return true;
};

bool State::checkCollections() {
  if (!_collectionsChecked) {
    _collectionsChecked =
        checkCollection("log") && checkCollection("election");
  }
  return _collectionsChecked;
}

bool State::createCollections() {
  if (!_collectionsChecked) {
    return (createCollection("log") && createCollection("election"));
  }
  return _collectionsChecked;
}

bool State::checkCollection(std::string const& name) {
  if (!_collectionsChecked) {
    return (
      TRI_LookupCollectionByNameVocBase(_vocbase, name) != nullptr);
  }
  return true;
}

bool State::createCollection(std::string const& name) {
  Builder body;
  body.add(VPackValue(VPackValueType::Object));
  body.close();

  VocbaseCollectionInfo parameters(_vocbase, name.c_str(),
                                   TRI_COL_TYPE_DOCUMENT, body.slice());
  TRI_vocbase_col_t const* collection =
      TRI_CreateCollectionVocBase(_vocbase, parameters, parameters.id(), true);
  
  if (collection == nullptr) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_errno(), "cannot create collection");
  }

  return true;

}

bool State::loadCollections(TRI_vocbase_t* vocbase, bool waitForSync) {
  _vocbase = vocbase;

  _options.waitForSync = waitForSync;
  _options.silent = true;

  return loadCollection("log");
}

bool State::loadCollection(std::string const& name) {
  TRI_ASSERT(_vocbase != nullptr);
  
  if (checkCollection(name)) {
    auto bindVars = std::make_shared<VPackBuilder>();
    bindVars->openObject();
    bindVars->close();
    
    std::string const aql(std::string("FOR l IN ") + name
                          + " SORT l._key RETURN l");
    arangodb::aql::Query query(false, _vocbase,
                               aql.c_str(), aql.size(), bindVars, nullptr,
                               arangodb::aql::PART_MAIN);
    
    auto queryResult = query.execute(QueryRegistryFeature::QUERY_REGISTRY);
    
    if (queryResult.code != TRI_ERROR_NO_ERROR) {
      THROW_ARANGO_EXCEPTION_MESSAGE(queryResult.code, queryResult.details);
    }
    
    VPackSlice result = queryResult.result->slice();
        
    if (result.isArray()) {
      for (auto const& i : VPackArrayIterator(result)) {
        buffer_t tmp =
              std::make_shared<arangodb::velocypack::Buffer<uint8_t>>();
        VPackSlice req = i.get("request");
        tmp->append(req.startAs<char const>(), req.byteSize());
        _log.push_back(
          log_t(std::stoi(i.get(TRI_VOC_ATTRIBUTE_KEY).copyString()),
                static_cast<term_t>(i.get("term").getUInt()),
                static_cast<arangodb::consensus::id_t>(i.get("leader").getUInt()), tmp));
      }
    }

    return true;
  } 
  
  LOG_TOPIC (INFO, Logger::AGENCY) << "Couldn't find persisted log";
  createCollections();

  return false;
}

bool State::find (arangodb::consensus::index_t prevIndex, term_t prevTerm) {
  MUTEX_LOCKER(mutexLocker, _logLock);
  if (prevIndex > _log.size()) {
    return false;
  }
  return _log.at(prevIndex).term == prevTerm;
}

bool State::compact () {

  // get read db at lastcommit % n == 0
  // save read db with key 10
  // update offset in logs
  // delete

  return true;
  
}
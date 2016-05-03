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
/// @author Andreas Streichardt
////////////////////////////////////////////////////////////////////////////////

#ifndef ARANGODB_CLUSTER_AGENCYCALLBACK_H
#define ARANGODB_CLUSTER_AGENCYCALLBACK_H

#include "Basics/ConditionVariable.h"
#include "Basics/Mutex.h"

#include <functional>
#include <memory>
#include <velocypack/Builder.h>
#include <velocypack/velocypack-aliases.h>

#include "Cluster/AgencyComm.h"

namespace arangodb {

class AgencyCallback {
public:
  //////////////////////////////////////////////////////////////////////////////
  /// @brief ctor
  //////////////////////////////////////////////////////////////////////////////
  AgencyCallback(AgencyComm&, std::string const&, 
                 std::function<bool(VPackSlice const&)> const&, bool needsValue,
                 bool needsInitialValue = true);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief wait a specified timeout. execute cb if watch didn't fire
  //////////////////////////////////////////////////////////////////////////////
  void waitWithFailover(double timeout);
  
  std::string const key;
  
  void refetchAndUpdate();
  void waitForExecution(double);
  

private:
  arangodb::Mutex _lock;
  arangodb::basics::ConditionVariable _cv;
  bool _useCv;

  AgencyComm& _agency;
  std::function<bool(VPackSlice const&)> const _cb;
  std::shared_ptr<VPackBuilder> _lastData;
  bool const _needsValue;

  // execute callback with current value data
  bool execute(std::shared_ptr<VPackBuilder>);
  // execute callback without any data
  bool executeEmpty(); 

  void checkValue(std::shared_ptr<VPackBuilder>);
};

}

#endif
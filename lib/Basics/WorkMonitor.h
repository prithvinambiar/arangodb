////////////////////////////////////////////////////////////////////////////////
/// @brief work monitor class
///
/// @file
///
/// DISCLAIMER
///
/// Copyright 2015 ArangoDB GmbH, Cologne, Germany
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
/// @author Dr. Frank Celler
/// @author Copyright 2015, ArangoDB GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

#ifndef ARANGODB_UTILS_WORK_MONITOR
#define ARANGODB_UTILS_WORK_MONITOR 1

#include "Basics/Thread.h"

#include "Basics/WorkItem.h"

// -----------------------------------------------------------------------------
// --SECTION--                                              forward declarations
// -----------------------------------------------------------------------------

namespace triagens {
namespace rest {
class HttpHandler;
}
}

namespace arangodb {
namespace velocypack {
class Builder;
}

// -----------------------------------------------------------------------------
// --SECTION--                                               enum class WorkType
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief type of the current work
////////////////////////////////////////////////////////////////////////////////

enum class WorkType { THREAD, HANDLER, CUSTOM };

// -----------------------------------------------------------------------------
// --SECTION--                                            struct WorkDescription
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief description of the current work
////////////////////////////////////////////////////////////////////////////////

struct WorkDescription {
  WorkDescription(WorkType, WorkDescription*);

  WorkType _type;
  bool _destroy;

  char _customType[16];

  union data {
    data() {}
    ~data() {}

    char text[256];
    triagens::basics::Thread* thread;
    triagens::rest::HttpHandler* handler;
  } _data;

  WorkDescription* _prev;
};

// -----------------------------------------------------------------------------
// --SECTION--                                                 class WorkMonitor
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief work monitor class
////////////////////////////////////////////////////////////////////////////////

class WorkMonitor : public triagens::basics::Thread {
  // -----------------------------------------------------------------------------
  // --SECTION--                                      constructors and
  // destructors
  // -----------------------------------------------------------------------------

 public:
  ////////////////////////////////////////////////////////////////////////////////
  /// @brief constructors a new monitor
  ////////////////////////////////////////////////////////////////////////////////

  WorkMonitor();

  // -----------------------------------------------------------------------------
  // --SECTION--                                             static public
  // methods
  // -----------------------------------------------------------------------------

 public:
  ////////////////////////////////////////////////////////////////////////////////
  /// @brief creates an empty WorkDescription
  ////////////////////////////////////////////////////////////////////////////////

  static WorkDescription* createWorkDescription(WorkType);

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief activates a WorkDescription
  ////////////////////////////////////////////////////////////////////////////////

  static void activateWorkDescription(WorkDescription*);

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief deactivates a WorkDescription
  ////////////////////////////////////////////////////////////////////////////////

  static WorkDescription* deactivateWorkDescription();

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief frees an WorkDescription
  ////////////////////////////////////////////////////////////////////////////////

  static void freeWorkDescription(WorkDescription* desc);

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief pushes a threads
  ////////////////////////////////////////////////////////////////////////////////

  static void pushThread(triagens::basics::Thread* thread);

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief pops a threads
  ////////////////////////////////////////////////////////////////////////////////

  static void popThread(triagens::basics::Thread* thread);

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief pushes a custom task
  ////////////////////////////////////////////////////////////////////////////////

  static void pushCustom(const char* type, const char* text, size_t length);

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief pops a custom task
  ////////////////////////////////////////////////////////////////////////////////

  static void popCustom();

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief pushes a handler
  ////////////////////////////////////////////////////////////////////////////////

  static void pushHandler(triagens::rest::HttpHandler*);

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief pops and releases a handler
  ////////////////////////////////////////////////////////////////////////////////

  static WorkDescription* popHandler(triagens::rest::HttpHandler*, bool free);

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief handler deleter
  ////////////////////////////////////////////////////////////////////////////////

  static void DELETE_HANDLER(WorkDescription* desc);

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief handler description string
  ////////////////////////////////////////////////////////////////////////////////

  static void VPACK_HANDLER(arangodb::velocypack::Builder*,
                            WorkDescription* desc);

  // -----------------------------------------------------------------------------
  // --SECTION--                                                    Thread
  // methods
  // -----------------------------------------------------------------------------

 protected:
  ////////////////////////////////////////////////////////////////////////////////
  /// {@inheritDoc}
  ////////////////////////////////////////////////////////////////////////////////

  void run() override;

  // -----------------------------------------------------------------------------
  // --SECTION--                                                    public
  // methods
  // -----------------------------------------------------------------------------

 public:
  ////////////////////////////////////////////////////////////////////////////////
  /// @brief initiate shutdown
  ////////////////////////////////////////////////////////////////////////////////

  void shutdown() { _stopping = true; }

  // -----------------------------------------------------------------------------
  // --SECTION--                                                 private
  // variables
  // -----------------------------------------------------------------------------

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief stopping flag
  ////////////////////////////////////////////////////////////////////////////////

  std::atomic<bool> _stopping;
};

// -----------------------------------------------------------------------------
// --SECTION--                                            class HandlerWorkStack
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief auto push and pop for HttpHandler
////////////////////////////////////////////////////////////////////////////////

class HandlerWorkStack {
  HandlerWorkStack(const HandlerWorkStack&) = delete;
  HandlerWorkStack& operator=(const HandlerWorkStack&) = delete;

  // -----------------------------------------------------------------------------
  // --SECTION--                                      constructors and
  // destructors
  // -----------------------------------------------------------------------------

 public:
  ////////////////////////////////////////////////////////////////////////////////
  /// @brief constructor
  ////////////////////////////////////////////////////////////////////////////////

  explicit HandlerWorkStack(triagens::rest::HttpHandler*);

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief constructor
  ////////////////////////////////////////////////////////////////////////////////

  explicit HandlerWorkStack(WorkItem::uptr<triagens::rest::HttpHandler>&);

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief destructor
  ////////////////////////////////////////////////////////////////////////////////

  ~HandlerWorkStack();

  // -----------------------------------------------------------------------------
  // --SECTION--                                                    public
  // methods
  // -----------------------------------------------------------------------------

 public:
  ////////////////////////////////////////////////////////////////////////////////
  /// @brief returns the handler
  ////////////////////////////////////////////////////////////////////////////////

  triagens::rest::HttpHandler* handler() const { return _handler; }

  // -----------------------------------------------------------------------------
  // --SECTION--                                                   private
  // members
  // -----------------------------------------------------------------------------

 private:
  ////////////////////////////////////////////////////////////////////////////////
  /// @brief handler
  ////////////////////////////////////////////////////////////////////////////////

  triagens::rest::HttpHandler* _handler;
};

// -----------------------------------------------------------------------------
// --SECTION--                                             class CustomWorkStack
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief auto push and pop for HttpHandler
////////////////////////////////////////////////////////////////////////////////

class CustomWorkStack {
  CustomWorkStack(const CustomWorkStack&) = delete;
  CustomWorkStack& operator=(const CustomWorkStack&) = delete;

  // -----------------------------------------------------------------------------
  // --SECTION--                                      constructors and
  // destructors
  // -----------------------------------------------------------------------------

 public:
  ////////////////////////////////////////////////////////////////////////////////
  /// @brief constructor
  ////////////////////////////////////////////////////////////////////////////////

  CustomWorkStack(const char* type, const char* text, size_t length);

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief destructory
  ////////////////////////////////////////////////////////////////////////////////

  ~CustomWorkStack();
};

// -----------------------------------------------------------------------------
// --SECTION--                                                    module methods
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief starts the work monitor
////////////////////////////////////////////////////////////////////////////////

void InitializeWorkMonitor();

////////////////////////////////////////////////////////////////////////////////
/// @brief stops the work monitor
////////////////////////////////////////////////////////////////////////////////

void ShutdownWorkMonitor();
}

#endif

// -----------------------------------------------------------------------------
// --SECTION--                                                       END-OF-FILE
// -----------------------------------------------------------------------------

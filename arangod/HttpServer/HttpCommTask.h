////////////////////////////////////////////////////////////////////////////////
/// @brief http communication
///
/// @file
///
/// DISCLAIMER
///
/// Copyright 2014-2015 ArangoDB GmbH, Cologne, Germany
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
/// @author Dr. Frank Celler
/// @author Achim Brandt
/// @author Copyright 2014-2015, ArangoDB GmbH, Cologne, Germany
/// @author Copyright 2009-2013, triAGENS GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

#ifndef ARANGODB_HTTP_SERVER_HTTP_COMM_TASK_H
#define ARANGODB_HTTP_SERVER_HTTP_COMM_TASK_H 1

#include "Scheduler/SocketTask.h"

#include "Basics/Mutex.h"
#include "Basics/StringBuffer.h"
#include "Basics/WorkItem.h"

#include <deque>

namespace triagens {
namespace rest {
class HttpCommTask;
class HttpHandler;
class HttpRequest;
class HttpResponse;
class HttpServer;
class HttpServerJob;

// -----------------------------------------------------------------------------
// --SECTION--                                                class HttpCommTask
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief http communication
////////////////////////////////////////////////////////////////////////////////

class HttpCommTask : public SocketTask, public RequestStatisticsAgent {
  HttpCommTask(HttpCommTask const&) = delete;
  HttpCommTask const& operator=(HttpCommTask const&) = delete;

  // -----------------------------------------------------------------------------
  // --SECTION--                                      constructors and
  // destructors
  // -----------------------------------------------------------------------------

 public:
  ////////////////////////////////////////////////////////////////////////////////
  /// @brief constructs a new task
  ////////////////////////////////////////////////////////////////////////////////

  HttpCommTask(HttpServer*, TRI_socket_t, const ConnectionInfo&,
               double keepAliveTimeout);

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief destructs a task
  ////////////////////////////////////////////////////////////////////////////////

 protected:
  ~HttpCommTask();

  // -----------------------------------------------------------------------------
  // --SECTION--                                                    public
  // methods
  // -----------------------------------------------------------------------------

 public:
  ////////////////////////////////////////////////////////////////////////////////
  /// @brief handles response
  ////////////////////////////////////////////////////////////////////////////////

  void handleResponse(HttpResponse*);

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief reads data from the socket
  ////////////////////////////////////////////////////////////////////////////////

  bool processRead();

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief sends more chunked data
  ////////////////////////////////////////////////////////////////////////////////

  void sendChunk(basics::StringBuffer*);

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief chunking is finished
  ////////////////////////////////////////////////////////////////////////////////

  void finishedChunked();

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief task set up complete
  ////////////////////////////////////////////////////////////////////////////////

  void setupDone();

  // -----------------------------------------------------------------------------
  // --SECTION--                                                   private
  // methods
  // -----------------------------------------------------------------------------

 private:
  ////////////////////////////////////////////////////////////////////////////////
  /// @brief reads data from the socket
  ////////////////////////////////////////////////////////////////////////////////

  void addResponse(HttpResponse*);

  ////////////////////////////////////////////////////////////////////////////////
  /// check the content-length header of a request and fail it is broken
  ////////////////////////////////////////////////////////////////////////////////

  bool checkContentLength(bool expectContentLength);

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief fills the write buffer
  ////////////////////////////////////////////////////////////////////////////////

  void fillWriteBuffer();

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief handles CORS options
  ////////////////////////////////////////////////////////////////////////////////

  void processCorsOptions(uint32_t compatibility);

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief processes a request
  ////////////////////////////////////////////////////////////////////////////////

  void processRequest(uint32_t compatibility);

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief clears the request object
  ////////////////////////////////////////////////////////////////////////////////

  void clearRequest();

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief resets the internal state
  ///
  /// this method can be called to clean up when the request handling aborts
  /// prematurely
  ////////////////////////////////////////////////////////////////////////////////

  void resetState(bool close);

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief decides whether or not we should send back a www-authenticate
  /// header
  ////////////////////////////////////////////////////////////////////////////////

  bool sendWwwAuthenticateHeader() const;

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief get request compatibility
  ////////////////////////////////////////////////////////////////////////////////

  int32_t getCompatibility() const;

  // -----------------------------------------------------------------------------
  // --SECTION--                                                      Task
  // methods
  // -----------------------------------------------------------------------------

 protected:
  ////////////////////////////////////////////////////////////////////////////////
  /// {@inheritDoc}
  ////////////////////////////////////////////////////////////////////////////////

  bool setup(Scheduler* scheduler, EventLoop loop) override;

  ////////////////////////////////////////////////////////////////////////////////
  /// {@inheritDoc}
  ////////////////////////////////////////////////////////////////////////////////

  void cleanup() override;

  ////////////////////////////////////////////////////////////////////////////////
  /// {@inheritDoc}
  ////////////////////////////////////////////////////////////////////////////////

  bool handleEvent(EventToken token, EventType events) override;

  ////////////////////////////////////////////////////////////////////////////////
  /// {@inheritDoc}
  ////////////////////////////////////////////////////////////////////////////////

  void signalTask(TaskData*) override;

  // -----------------------------------------------------------------------------
  // --SECTION--                                                SocketTask
  // methods
  // -----------------------------------------------------------------------------

 protected:
  ////////////////////////////////////////////////////////////////////////////////
  /// {@inheritDoc}
  ////////////////////////////////////////////////////////////////////////////////

  bool handleRead() override;

  ////////////////////////////////////////////////////////////////////////////////
  /// {@inheritDoc}
  ////////////////////////////////////////////////////////////////////////////////

  void completedWriteBuffer() override;

  ////////////////////////////////////////////////////////////////////////////////
  /// {@inheritDoc}
  ////////////////////////////////////////////////////////////////////////////////

  void handleTimeout() override;

  // -----------------------------------------------------------------------------
  // --SECTION--                                               protected
  // variables
  // -----------------------------------------------------------------------------

 protected:
  ////////////////////////////////////////////////////////////////////////////////
  /// @brief connection info
  ////////////////////////////////////////////////////////////////////////////////

  ConnectionInfo _connectionInfo;

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief the underlying server
  ////////////////////////////////////////////////////////////////////////////////

  HttpServer* const _server;

  // -----------------------------------------------------------------------------
  // --SECTION--                                                 private
  // variables
  // -----------------------------------------------------------------------------

 private:
  ////////////////////////////////////////////////////////////////////////////////
  /// @brief write buffers
  ////////////////////////////////////////////////////////////////////////////////

  std::deque<basics::StringBuffer*> _writeBuffers;

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief statistics buffers
  ////////////////////////////////////////////////////////////////////////////////

  std::deque<TRI_request_statistics_t*> _writeBuffersStats;

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief current read position
  ////////////////////////////////////////////////////////////////////////////////

  size_t _readPosition;

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief start of the body position
  ////////////////////////////////////////////////////////////////////////////////

  size_t _bodyPosition;

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief body length
  ////////////////////////////////////////////////////////////////////////////////

  size_t _bodyLength;

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief true if request is complete but not handled
  ////////////////////////////////////////////////////////////////////////////////

  bool _requestPending;

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief true if a close has been requested by the client
  ////////////////////////////////////////////////////////////////////////////////

  bool _closeRequested;

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief true if reading the request body
  ////////////////////////////////////////////////////////////////////////////////

  bool _readRequestBody;

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief whether or not to allow credentialed requests
  ///
  /// this is only used for CORS
  ////////////////////////////////////////////////////////////////////////////////

  bool _denyCredentials;

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief whether the client accepts deflate algorithm
  ////////////////////////////////////////////////////////////////////////////////

  bool _acceptDeflate;

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief new request started
  ////////////////////////////////////////////////////////////////////////////////

  bool _newRequest;

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief true if within a chunked response
  ////////////////////////////////////////////////////////////////////////////////

  bool _isChunked;

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief the request with possible incomplete body
  ////////////////////////////////////////////////////////////////////////////////

  HttpRequest* _request;

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief http version number used
  ////////////////////////////////////////////////////////////////////////////////

  HttpRequest::HttpVersion _httpVersion;

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief type of request (GET, POST, ...)
  ////////////////////////////////////////////////////////////////////////////////

  HttpRequest::HttpRequestType _requestType;

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief value of requested URL
  ////////////////////////////////////////////////////////////////////////////////

  std::string _fullUrl;

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief value of the HTTP origin header the client sent (if any).
  ///
  /// this is only used for CORS
  ////////////////////////////////////////////////////////////////////////////////

  std::string _origin;

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief start position of current request
  ////////////////////////////////////////////////////////////////////////////////

  size_t _startPosition;

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief number of requests since last compactification
  ////////////////////////////////////////////////////////////////////////////////

  size_t _sinceCompactification;

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief original body length
  ////////////////////////////////////////////////////////////////////////////////

  size_t _originalBodyLength;

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief task ready
  ////////////////////////////////////////////////////////////////////////////////

  std::atomic<bool> _setupDone;

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief the maximal header size
  ////////////////////////////////////////////////////////////////////////////////

  static size_t const MaximalHeaderSize;

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief the maximal body size
  ////////////////////////////////////////////////////////////////////////////////

  static size_t const MaximalBodySize;

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief the maximal pipeline size
  ////////////////////////////////////////////////////////////////////////////////

  static size_t const MaximalPipelineSize;
};
}
}

#endif

// -----------------------------------------------------------------------------
// --SECTION--                                                       END-OF-FILE
// -----------------------------------------------------------------------------

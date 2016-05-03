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
/// @author Dr. Frank Celler
////////////////////////////////////////////////////////////////////////////////

#ifndef ARANGODB_REST_GENERAL_RESPONSE_H
#define ARANGODB_REST_GENERAL_RESPONSE_H 1

#include "Basics/Common.h"

#include "Basics/StaticStrings.h"

namespace arangodb {
class GeneralResponse {
  GeneralResponse() = delete;
  GeneralResponse(GeneralResponse const&) = delete;
  GeneralResponse& operator=(GeneralResponse const&) = delete;

 public:
  enum class ResponseCode {
    CONTINUE = 100,
    SWITCHING_PROTOCOLS = 101,
    PROCESSING = 102,

    OK = 200,
    CREATED = 201,
    ACCEPTED = 202,
    PARTIAL = 203,
    NO_CONTENT = 204,
    RESET_CONTENT = 205,
    PARTIAL_CONTENT = 206,

    MOVED_PERMANENTLY = 301,
    FOUND = 302,
    SEE_OTHER = 303,
    NOT_MODIFIED = 304,
    TEMPORARY_REDIRECT = 307,
    PERMANENT_REDIRECT = 308,

    BAD = 400,
    UNAUTHORIZED = 401,
    PAYMENT_REQUIRED = 402,
    FORBIDDEN = 403,
    NOT_FOUND = 404,
    METHOD_NOT_ALLOWED = 405,
    NOT_ACCEPTABLE = 406,
    REQUEST_TIMEOUT = 408,
    CONFLICT = 409,
    GONE = 410,
    LENGTH_REQUIRED = 411,
    PRECONDITION_FAILED = 412,
    REQUEST_ENTITY_TOO_LARGE = 413,
    REQUEST_URI_TOO_LONG = 414,
    UNSUPPORTED_MEDIA_TYPE = 415,
    REQUESTED_RANGE_NOT_SATISFIABLE = 416,
    EXPECTATION_FAILED = 417,
    I_AM_A_TEAPOT = 418,
    UNPROCESSABLE_ENTITY = 422,
    LOCKED = 423,
    PRECONDITION_REQUIRED = 428,
    TOO_MANY_REQUESTS = 429,
    REQUEST_HEADER_FIELDS_TOO_LARGE = 431,
    UNAVAILABLE_FOR_LEGAL_REASONS = 451,

    SERVER_ERROR = 500,
    NOT_IMPLEMENTED = 501,
    BAD_GATEWAY = 502,
    SERVICE_UNAVAILABLE = 503,
    HTTP_VERSION_NOT_SUPPORTED = 505,
    BANDWIDTH_LIMIT_EXCEEDED = 509,
    NOT_EXTENDED = 510
  };

 public:
  // converts the response code to a string for delivering to a http client.
  static std::string responseString(ResponseCode);

  // converts the response code string to the internal code
  static ResponseCode responseCode(std::string const& str);

  // response code from integer error code
  static ResponseCode responseCode(int);

 public:
  GeneralResponse(ResponseCode, uint32_t);
  virtual ~GeneralResponse() {}

 public:
  ResponseCode responseCode() const { return _responseCode; }
  void setResponseCode(ResponseCode responseCode) {
    _responseCode = responseCode;
  }

  void setContentType(std::string const& contentType) {
    _headers[arangodb::StaticStrings::ContentTypeHeader] = contentType;
  }

  // Returns the value of a header field with given name. If no header field
  // with the given name was specified by the client, the empty string is
  // returned.
  std::string const& header(std::string const& field) const;
  std::string const& header(std::string const&, bool& found) const;
  std::unordered_map<std::string, std::string> headers() const { return _headers; }

  void setHeader(std::string const& key, std::string const& value);

  // the header field name must already be trimmed and lower-cased
  void setHeaderNC(std::string const& key, std::string const& value);
  void setHeaderNC(std::string const& key, std::string&& value);

 private:
  // checks for special headers
  virtual void checkHeader(std::string const& key, std::string const& value) {}

 protected:
  ResponseCode _responseCode;
  uint32_t const _apiCompatibility;
  std::unordered_map<std::string, std::string> _headers;
};
}

#endif
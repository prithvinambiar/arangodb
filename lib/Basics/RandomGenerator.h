////////////////////////////////////////////////////////////////////////////////
/// @brief collection of random functions and classes
///
/// @file
/// Thread-safe random generator
///
/// DISCLAIMER
///
/// Copyright 2014 ArangoDB GmbH, Cologne, Germany
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
/// @author Copyright 2014, ArangoDB GmbH, Cologne, Germany
/// @author Copyright 2009-2013, triAGENS GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

#ifndef ARANGODB_BASICS_RANDOM_GENERATOR_H
#define ARANGODB_BASICS_RANDOM_GENERATOR_H 1

#include "Basics/Common.h"

#ifdef _WIN32
#include <Wincrypt.h>
#endif

namespace triagens {
namespace basics {

////////////////////////////////////////////////////////////////////////////////
/// @brief collection of random functions and classes
////////////////////////////////////////////////////////////////////////////////

namespace Random {

////////////////////////////////////////////////////////////////////////////////
/// @brief type of the random generator
////////////////////////////////////////////////////////////////////////////////

enum random_e {
  RAND_MERSENNE = 1,
  RAND_RANDOM = 2,
  RAND_URANDOM = 3,
  RAND_COMBINED = 4,
  RAND_WIN32 = 5  // uses the built in cryptographic services offered and
                  // recommended by microsoft (e.g. CryptGenKey(...) )
};

////////////////////////////////////////////////////////////////////////////////
/// @ingroup Utilities
/// @brief randomize random
////////////////////////////////////////////////////////////////////////////////

void seed();

////////////////////////////////////////////////////////////////////////////////
/// @ingroup Utilities
/// @brief selects random generator
////////////////////////////////////////////////////////////////////////////////

random_e selectVersion(random_e);

////////////////////////////////////////////////////////////////////////////////
/// @ingroup Utilities
/// @brief returns random generator version
////////////////////////////////////////////////////////////////////////////////

random_e currentVersion();

////////////////////////////////////////////////////////////////////////////////
/// @ingroup Utilities
/// @brief static destructor for all allocated rngs. frees file descriptors
////////////////////////////////////////////////////////////////////////////////

void shutdown();

////////////////////////////////////////////////////////////////////////////////
/// @ingroup Utilities
/// @brief returns true if random generator might block
////////////////////////////////////////////////////////////////////////////////

bool isBlocking();

////////////////////////////////////////////////////////////////////////////////
/// @ingroup Utilities
/// @brief interval inclusive both margins
////////////////////////////////////////////////////////////////////////////////

int32_t interval(int32_t left, int32_t right);

////////////////////////////////////////////////////////////////////////////////
/// @ingroup Utilities
/// @brief interval inclusive both margins
////////////////////////////////////////////////////////////////////////////////

uint32_t interval(uint32_t left, uint32_t right);

// -----------------------------------------------------------------------------
// uniform integer generator
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @ingroup Utilities
/// @brief constructor, the range includes both left and right
////////////////////////////////////////////////////////////////////////////////

class UniformInteger {
 private:
  UniformInteger(UniformInteger const&);
  UniformInteger& operator=(UniformInteger const&);

 public:
  ////////////////////////////////////////////////////////////////////////////////
  /// @brief constructor, the range includes both margins
  ////////////////////////////////////////////////////////////////////////////////

  UniformInteger(int32_t left, int32_t right) : left(left), right(right) {}

 public:
  ////////////////////////////////////////////////////////////////////////////////
  /// @brief returns a random integer between left and right inclusive
  ////////////////////////////////////////////////////////////////////////////////

  int32_t random();

 private:
  int32_t left;
  int32_t right;
};

// -----------------------------------------------------------------------------
// uniform character generator
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @ingroup Utilities
/// @brief uniform character string
////////////////////////////////////////////////////////////////////////////////

class UniformCharacter {
 private:
  UniformCharacter(UniformCharacter const&);
  UniformCharacter& operator=(UniformCharacter const&);

 public:
  ////////////////////////////////////////////////////////////////////////////////
  /// @brief constructor
  ////////////////////////////////////////////////////////////////////////////////

  explicit UniformCharacter(size_t length);

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief constructor
  ////////////////////////////////////////////////////////////////////////////////

  explicit UniformCharacter(std::string const& characters);

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief constructor
  ////////////////////////////////////////////////////////////////////////////////

  UniformCharacter(size_t length, std::string const& characters);

 public:
  ////////////////////////////////////////////////////////////////////////////////
  /// @brief returns a random string of fixed length
  ////////////////////////////////////////////////////////////////////////////////

  std::string random();

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief returns a random string of given length
  ////////////////////////////////////////////////////////////////////////////////

  std::string random(size_t length);

 private:
  size_t length;
  std::string const characters;
  UniformInteger generator;
};
}
}
}

#endif
// -----------------------------------------------------------------------------
// --SECTION--                                                       END-OF-FILE
// -----------------------------------------------------------------------------

// Local Variables:
// mode: outline-minor
// outline-regexp: "/// @brief\\|/// {@inheritDoc}\\|/// @page\\|//
// --SECTION--\\|/// @\\}"
// End:

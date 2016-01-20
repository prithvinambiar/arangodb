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
/// @author Jan Steemann
////////////////////////////////////////////////////////////////////////////////

#include "Index.h"
#include "Aql/Ast.h"
#include "Aql/AstNode.h"
#include "Aql/Variable.h"
#include "Basics/Exceptions.h"
#include "Basics/JsonHelper.h"
#include "Basics/json.h"
#include "Basics/json-utilities.h"
#include "Basics/StringUtils.h"
#include "VocBase/server.h"
#include "VocBase/VocShaper.h"

#include <ostream>

using namespace arangodb;

Index::Index(
    TRI_idx_iid_t iid, TRI_document_collection_t* collection,
    std::vector<std::vector<arangodb::basics::AttributeName>> const& fields,
    bool unique, bool sparse)
    : _iid(iid),
      _collection(collection),
      _fields(fields),
      _unique(unique),
      _sparse(sparse),
      _selectivityEstimate(0.0) {
  // note: _collection can be a nullptr in the cluster coordinator case!!
  // note: _selectivityEstimate is only used in cluster coordinator case
}

////////////////////////////////////////////////////////////////////////////////
/// @brief create an index stub with a hard-coded selectivity estimate
/// this is used in the cluster coordinator case
////////////////////////////////////////////////////////////////////////////////

Index::Index(TRI_json_t const* json)
    : _iid(arangodb::basics::StringUtils::uint64(
          arangodb::basics::JsonHelper::checkAndGetStringValue(json, "id"))),
      _collection(nullptr),
      _fields(),
      _unique(
          arangodb::basics::JsonHelper::getBooleanValue(json, "unique", false)),
      _sparse(
          arangodb::basics::JsonHelper::getBooleanValue(json, "sparse", false)),
      _selectivityEstimate(0.0) {
  TRI_json_t const* fields = TRI_LookupObjectJson(json, "fields");

  if (!TRI_IsArrayJson(fields)) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL,
                                   "invalid index description");
  }

  size_t const n = TRI_LengthArrayJson(fields);
  _fields.reserve(n);

  for (size_t i = 0; i < n; ++i) {
    auto name = static_cast<TRI_json_t const*>(
        TRI_AtVector(&fields->_value._objects, i));

    if (!TRI_IsStringJson(name)) {
      THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL,
                                     "invalid index description");
    }

    std::vector<arangodb::basics::AttributeName> parsedAttributes;
    TRI_ParseAttributeString(
        std::string(name->_value._string.data, name->_value._string.length - 1),
        parsedAttributes);
    _fields.emplace_back(parsedAttributes);
  }

  TRI_json_t const* se = TRI_LookupObjectJson(json, "selectivityEstimate");

  if (TRI_IsNumberJson(se)) {
    _selectivityEstimate = json->_value._number;
  }
}

Index::~Index() {}


////////////////////////////////////////////////////////////////////////////////
/// @brief return the index type based on a type name
////////////////////////////////////////////////////////////////////////////////

Index::IndexType Index::type(char const* type) {
  if (::strcmp(type, "primary") == 0) {
    return TRI_IDX_TYPE_PRIMARY_INDEX;
  }
  if (::strcmp(type, "edge") == 0) {
    return TRI_IDX_TYPE_EDGE_INDEX;
  }
  if (::strcmp(type, "hash") == 0) {
    return TRI_IDX_TYPE_HASH_INDEX;
  }
  if (::strcmp(type, "skiplist") == 0) {
    return TRI_IDX_TYPE_SKIPLIST_INDEX;
  }
  if (::strcmp(type, "fulltext") == 0) {
    return TRI_IDX_TYPE_FULLTEXT_INDEX;
  }
  if (::strcmp(type, "cap") == 0) {
    return TRI_IDX_TYPE_CAP_CONSTRAINT;
  }
  if (::strcmp(type, "geo1") == 0) {
    return TRI_IDX_TYPE_GEO1_INDEX;
  }
  if (::strcmp(type, "geo2") == 0) {
    return TRI_IDX_TYPE_GEO2_INDEX;
  }

  return TRI_IDX_TYPE_UNKNOWN;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief return the name of an index type
////////////////////////////////////////////////////////////////////////////////

char const* Index::typeName(Index::IndexType type) {
  switch (type) {
    case TRI_IDX_TYPE_PRIMARY_INDEX:
      return "primary";
    case TRI_IDX_TYPE_EDGE_INDEX:
      return "edge";
    case TRI_IDX_TYPE_HASH_INDEX:
      return "hash";
    case TRI_IDX_TYPE_SKIPLIST_INDEX:
      return "skiplist";
    case TRI_IDX_TYPE_FULLTEXT_INDEX:
      return "fulltext";
    case TRI_IDX_TYPE_CAP_CONSTRAINT:
      return "cap";
    case TRI_IDX_TYPE_GEO1_INDEX:
      return "geo1";
    case TRI_IDX_TYPE_GEO2_INDEX:
      return "geo2";
    case TRI_IDX_TYPE_PRIORITY_QUEUE_INDEX:
    case TRI_IDX_TYPE_BITARRAY_INDEX:
    case TRI_IDX_TYPE_UNKNOWN: {
    }
  }

  return "";
}

////////////////////////////////////////////////////////////////////////////////
/// @brief validate an index id
////////////////////////////////////////////////////////////////////////////////

bool Index::validateId(char const* key) {
  char const* p = key;

  while (1) {
    char const c = *p;

    if (c == '\0') {
      return (p - key) > 0;
    }
    if (c >= '0' && c <= '9') {
      ++p;
      continue;
    }

    return false;
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief validate an index handle (collection name + / + index id)
////////////////////////////////////////////////////////////////////////////////

bool Index::validateHandle(char const* key, size_t* split) {
  char const* p = key;
  char c = *p;

  // extract collection name

  if (!(c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) {
    return false;
  }

  ++p;

  while (1) {
    c = *p;

    if ((c == '_') || (c == '-') || (c >= '0' && c <= '9') ||
        (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
      ++p;
      continue;
    }

    if (c == '/') {
      break;
    }

    return false;
  }

  if (p - key > TRI_COL_NAME_LENGTH) {
    return false;
  }

  // store split position
  *split = p - key;
  ++p;

  // validate index id
  return validateId(p);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief generate a new index id
////////////////////////////////////////////////////////////////////////////////

TRI_idx_iid_t Index::generateId() { return TRI_NewTickServer(); }

////////////////////////////////////////////////////////////////////////////////
/// @brief index comparator, used by the coordinator to detect if two index
/// contents are the same
////////////////////////////////////////////////////////////////////////////////

bool Index::Compare(TRI_json_t const* lhs, TRI_json_t const* rhs) {
  TRI_json_t* typeJson = TRI_LookupObjectJson(lhs, "type");
  TRI_ASSERT(TRI_IsStringJson(typeJson));

  // type must be identical
  if (!TRI_CheckSameValueJson(typeJson, TRI_LookupObjectJson(rhs, "type"))) {
    return false;
  }

  auto type = Index::type(typeJson->_value._string.data);

  // unique must be identical if present
  TRI_json_t* value = TRI_LookupObjectJson(lhs, "unique");
  if (TRI_IsBooleanJson(value)) {
    if (!TRI_CheckSameValueJson(value, TRI_LookupObjectJson(rhs, "unique"))) {
      return false;
    }
  }

  // sparse must be identical if present
  value = TRI_LookupObjectJson(lhs, "sparse");
  if (TRI_IsBooleanJson(value)) {
    if (!TRI_CheckSameValueJson(value, TRI_LookupObjectJson(rhs, "sparse"))) {
      return false;
    }
  }

  if (type == IndexType::TRI_IDX_TYPE_GEO1_INDEX) {
    // geoJson must be identical if present
    value = TRI_LookupObjectJson(lhs, "geoJson");
    if (TRI_IsBooleanJson(value)) {
      if (!TRI_CheckSameValueJson(value,
                                  TRI_LookupObjectJson(rhs, "geoJson"))) {
        return false;
      }
    }
  } else if (type == IndexType::TRI_IDX_TYPE_FULLTEXT_INDEX) {
    // minLength
    value = TRI_LookupObjectJson(lhs, "minLength");
    if (TRI_IsNumberJson(value)) {
      if (!TRI_CheckSameValueJson(value,
                                  TRI_LookupObjectJson(rhs, "minLength"))) {
        return false;
      }
    }
  } else if (type == IndexType::TRI_IDX_TYPE_CAP_CONSTRAINT) {
    // size, byteSize
    value = TRI_LookupObjectJson(lhs, "size");
    if (TRI_IsNumberJson(value)) {
      if (!TRI_CheckSameValueJson(value, TRI_LookupObjectJson(rhs, "size"))) {
        return false;
      }
    }

    value = TRI_LookupObjectJson(lhs, "byteSize");
    if (TRI_IsNumberJson(value)) {
      if (!TRI_CheckSameValueJson(value,
                                  TRI_LookupObjectJson(rhs, "byteSize"))) {
        return false;
      }
    }
  }

  // other index types: fields must be identical if present
  value = TRI_LookupObjectJson(lhs, "fields");

  if (TRI_IsArrayJson(value)) {
    if (type == IndexType::TRI_IDX_TYPE_HASH_INDEX) {
      size_t const nv = TRI_LengthArrayJson(value);

      // compare fields in arbitrary order
      TRI_json_t const* r = TRI_LookupObjectJson(rhs, "fields");

      if (!TRI_IsArrayJson(r) || nv != TRI_LengthArrayJson(r)) {
        return false;
      }

      size_t const nr = TRI_LengthArrayJson(r);

      for (size_t i = 0; i < nv; ++i) {
        TRI_json_t const* v = TRI_LookupArrayJson(value, i);

        bool found = false;

        for (size_t j = 0; j < nr; ++j) {
          if (TRI_CheckSameValueJson(v, TRI_LookupArrayJson(r, j))) {
            found = true;
            break;
          }
        }

        if (!found) {
          return false;
        }
      }
    } else {
      if (!TRI_CheckSameValueJson(value, TRI_LookupObjectJson(rhs, "fields"))) {
        return false;
      }
    }
  }

  return true;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief return a contextual string for logging
////////////////////////////////////////////////////////////////////////////////

std::string Index::context() const {
  std::ostringstream result;

  result << "index { id: " << id() << ", type: " << typeName()
         << ", collection: " << _collection->_vocbase->_name << "/"
         << _collection->_info.name()
         << ", unique: " << (_unique ? "true" : "false") << ", fields: ";
  result << "[";
  for (size_t i = 0; i < _fields.size(); ++i) {
    if (i > 0) {
      result << ", ";
    }
    result << _fields[i];
  }
  result << "] }";

  return result.str();
}

////////////////////////////////////////////////////////////////////////////////
/// @brief create a VelocyPack representation of the index
/// base functionality (called from derived classes)
////////////////////////////////////////////////////////////////////////////////

std::shared_ptr<VPackBuilder> Index::toVelocyPack(bool withFigures,
                                                  bool closeToplevel) const {
  auto builder = std::make_shared<VPackBuilder>();
  builder->openObject();
  builder->add("id", VPackValue(std::to_string(_iid)));
  builder->add("type", VPackValue(typeName()));

  if (dumpFields()) {
    builder->add(VPackValue("fields"));
    VPackArrayBuilder b1(builder.get());

    for (auto const& field : fields()) {
      std::string fieldString;
      TRI_AttributeNamesToString(field, fieldString);
      builder->add(VPackValue(fieldString));
    }
  }

  if (hasSelectivityEstimate()) {
    builder->add("selectivityEstimate", VPackValue(selectivityEstimate()));
  }

  if (withFigures) {
    std::shared_ptr<VPackBuilder> figuresBuilder = toVelocyPackFigures(true);
    VPackSlice const figures = figuresBuilder->slice();
    builder->add("figures", figures);
  }
  if (closeToplevel) {
    builder->close();
  }
  return builder;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief create a VelocyPack representation of the index figures
/// base functionality (called from derived classes)
////////////////////////////////////////////////////////////////////////////////

std::shared_ptr<VPackBuilder> Index::toVelocyPackFigures(
    bool closeToplevel) const {
  auto builder = std::make_shared<VPackBuilder>();
  builder->openObject();
  builder->add("memory", VPackValue(memory()));
  if (closeToplevel) {
    builder->close();
  }
  return builder;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief create a JSON representation of the index
/// base functionality (called from derived classes)
////////////////////////////////////////////////////////////////////////////////

arangodb::basics::Json Index::toJson(TRI_memory_zone_t* zone,
                                     bool withFigures) const {
  arangodb::basics::Json json(zone, arangodb::basics::Json::Object, 4);

  json("id", arangodb::basics::Json(zone, std::to_string(_iid)))(
      "type", arangodb::basics::Json(zone, typeName()));

  if (dumpFields()) {
    arangodb::basics::Json f(zone, arangodb::basics::Json::Array,
                             fields().size());

    for (auto const& field : fields()) {
      std::string fieldString;
      TRI_AttributeNamesToString(field, fieldString);
      f.add(arangodb::basics::Json(zone, fieldString));
    }

    json("fields", f);
  }

  if (hasSelectivityEstimate()) {
    json("selectivityEstimate", arangodb::basics::Json(selectivityEstimate()));
  }

  if (withFigures) {
    json("figures", toJsonFigures(zone));
  }

  return json;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief create a JSON representation of the index figures
/// base functionality (called from derived classes)
////////////////////////////////////////////////////////////////////////////////

arangodb::basics::Json Index::toJsonFigures(TRI_memory_zone_t* zone) const {
  arangodb::basics::Json json(zone, arangodb::basics::Json::Object);
  json("memory", arangodb::basics::Json(static_cast<double>(memory())));
  return json;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief default implementation for selectivityEstimate
////////////////////////////////////////////////////////////////////////////////

double Index::selectivityEstimate() const {
  THROW_ARANGO_EXCEPTION(TRI_ERROR_NOT_IMPLEMENTED);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief default implementation for selectivityEstimate
////////////////////////////////////////////////////////////////////////////////

int Index::batchInsert(arangodb::Transaction*,
                       std::vector<TRI_doc_mptr_t const*> const*, size_t) {
  THROW_ARANGO_EXCEPTION(TRI_ERROR_NOT_IMPLEMENTED);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief default implementation for postInsert
////////////////////////////////////////////////////////////////////////////////

int Index::postInsert(arangodb::Transaction*,
                      struct TRI_transaction_collection_s*,
                      struct TRI_doc_mptr_t const*) {
  // do nothing
  return TRI_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief default implementation for cleanup
////////////////////////////////////////////////////////////////////////////////

int Index::cleanup() {
  // do nothing
  return TRI_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief default implementation for sizeHint
////////////////////////////////////////////////////////////////////////////////

int Index::sizeHint(arangodb::Transaction*, size_t) {
  // do nothing
  return TRI_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief default implementation for hasBatchInsert
////////////////////////////////////////////////////////////////////////////////

bool Index::hasBatchInsert() const { return false; }

////////////////////////////////////////////////////////////////////////////////
/// @brief default implementation for supportsFilterCondition
////////////////////////////////////////////////////////////////////////////////

bool Index::supportsFilterCondition(arangodb::aql::AstNode const* node,
                                    arangodb::aql::Variable const* reference,
                                    size_t itemsInIndex, size_t& estimatedItems,
                                    double& estimatedCost) const {
  // by default, no filter conditions are supported
  estimatedItems = itemsInIndex;
  estimatedCost = static_cast<double>(estimatedItems);
  return false;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief default implementation for supportsSortCondition
////////////////////////////////////////////////////////////////////////////////

bool Index::supportsSortCondition(arangodb::aql::SortCondition const*,
                                  arangodb::aql::Variable const*,
                                  size_t itemsInIndex,
                                  double& estimatedCost) const {
  // by default, no sort conditions are supported
  if (itemsInIndex > 0) {
    estimatedCost = itemsInIndex * std::log2(itemsInIndex);
  } else {
    estimatedCost = 0.0;
  }
  return false;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief default iterator factory method. does not create an iterator
////////////////////////////////////////////////////////////////////////////////

IndexIterator* Index::iteratorForCondition(
    arangodb::Transaction*, IndexIteratorContext*, arangodb::aql::Ast*,
    arangodb::aql::AstNode const*, arangodb::aql::Variable const*, bool) const {
  // the super class index cannot create an iterator
  // the derived index classes have to manage this.
  return nullptr;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief specializes the condition for use with the index
////////////////////////////////////////////////////////////////////////////////

arangodb::aql::AstNode* Index::specializeCondition(
    arangodb::aql::AstNode* node, arangodb::aql::Variable const*) const {
  return node;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief perform some base checks for an index condition part
////////////////////////////////////////////////////////////////////////////////

bool Index::canUseConditionPart(arangodb::aql::AstNode const* access,
                                arangodb::aql::AstNode const* other,
                                arangodb::aql::AstNode const* op,
                                arangodb::aql::Variable const* reference,
                                bool isExecution) const {
  if (_sparse) {
    if (op->type == arangodb::aql::NODE_TYPE_OPERATOR_BINARY_NIN) {
      return false;
    }

    if (op->type == arangodb::aql::NODE_TYPE_OPERATOR_BINARY_IN &&
        (other->type == arangodb::aql::NODE_TYPE_EXPANSION ||
         other->type == arangodb::aql::NODE_TYPE_ATTRIBUTE_ACCESS)) {
      // value IN a.b  OR  value IN a.b[*]
      if (!access->isConstant()) {
        return false;
      }

      /* A sparse index will store null in Array
      if (access->isNullValue()) {
        return false;
      }
      */
    } else if (op->type == arangodb::aql::NODE_TYPE_OPERATOR_BINARY_IN &&
               access->type == arangodb::aql::NODE_TYPE_EXPANSION) {
      // value[*] IN a.b
      if (!other->isConstant()) {
        return false;
      }

      /* A sparse index will store null in Array
      if (other->isNullValue()) {
        return false;
      }
      */
    } else if (access->type == arangodb::aql::NODE_TYPE_ATTRIBUTE_ACCESS) {
      // a.b == value  OR  a.b IN values
      if (!other->isConstant()) {
        return false;
      }

      if (op->type == arangodb::aql::NODE_TYPE_OPERATOR_BINARY_LT ||
          op->type == arangodb::aql::NODE_TYPE_OPERATOR_BINARY_LE) {
        // <  and  <= are not supported with sparse indexes as this may include
        // null values
        return false;
      }

      if (other->isNullValue() &&
          (op->type == arangodb::aql::NODE_TYPE_OPERATOR_BINARY_EQ ||
           op->type == arangodb::aql::NODE_TYPE_OPERATOR_BINARY_GE)) {
        // ==  and  >= null are not supported with sparse indexes for the same
        // reason
        return false;
      }

      if (op->type == arangodb::aql::NODE_TYPE_OPERATOR_BINARY_IN &&
          other->type == arangodb::aql::NODE_TYPE_ARRAY) {
        size_t const n = other->numMembers();

        for (size_t i = 0; i < n; ++i) {
          if (other->getMemberUnchecked(i)->isNullValue()) {
            return false;
          }
        }
      }
    }
  }

  if (isExecution) {
    // in execution phase, we do not need to check the variable usage again
    return true;
  }

  // test if the reference variable is contained on both side of the expression
  std::unordered_set<aql::Variable const*> variables;
  if (op->type == arangodb::aql::NODE_TYPE_OPERATOR_BINARY_IN &&
      (other->type == arangodb::aql::NODE_TYPE_EXPANSION ||
       other->type == arangodb::aql::NODE_TYPE_ATTRIBUTE_ACCESS)) {
    // value IN a.b  OR  value IN a.b[*]
    arangodb::aql::Ast::getReferencedVariables(access, variables);
  } else {
    // a.b == value  OR  a.b IN values
    arangodb::aql::Ast::getReferencedVariables(other, variables);
  }
  if (variables.find(reference) != variables.end()) {
    // yes. then we cannot use an index here
    return false;
  }

  return true;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief append the index description to an output stream
////////////////////////////////////////////////////////////////////////////////

std::ostream& operator<<(std::ostream& stream,
                         arangodb::Index const* index) {
  stream << index->context();
  return stream;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief append the index description to an output stream
////////////////////////////////////////////////////////////////////////////////

std::ostream& operator<<(std::ostream& stream,
                         arangodb::Index const& index) {
  stream << index.context();
  return stream;
}



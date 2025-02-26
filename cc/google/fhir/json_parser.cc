/*
 * Copyright 2018 Google LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <ctype.h>

#include <iosfwd>
#include <memory>
#include <unordered_map>
#include <utility>

#include "google/protobuf/any.pb.h"
#include "google/protobuf/descriptor.h"
#include "google/protobuf/message.h"
#include "absl/container/flat_hash_map.h"
#include "absl/memory/memory.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/synchronization/mutex.h"
#include "google/fhir/annotations.h"
#include "google/fhir/core_resource_registry.h"
#include "google/fhir/extensions.h"
#include "google/fhir/json_format.h"
#include "google/fhir/primitive_wrapper.h"
#include "google/fhir/proto_util.h"
#include "google/fhir/r4/profiles.h"
#include "google/fhir/references.h"
#include "google/fhir/resource_validation.h"
#include "google/fhir/status/status.h"
#include "google/fhir/status/statusor.h"
#include "google/fhir/stu3/profiles.h"
#include "google/fhir/util.h"
#include "proto/google/fhir/proto/annotations.pb.h"
#include "include/json/json.h"
#include "re2/re2.h"

namespace google {
namespace fhir {

using ::absl::InvalidArgumentError;
using ::google::fhir::IsChoiceType;
using ::google::fhir::IsPrimitive;
using ::google::fhir::IsReference;
using ::google::fhir::IsResource;
using ::google::fhir::proto::FhirVersion;
using ::google::protobuf::Any;
using ::google::protobuf::Descriptor;
using ::google::protobuf::FieldDescriptor;
using ::google::protobuf::Message;
using ::google::protobuf::Reflection;

namespace internal {

// Given a proto descriptor, constructs a map from FHIR JSON field name to
// FieldDescriptor.
// Since FHIR represents extensions to primitives as separate JSON fields,
// prepended by underscore, we add that as a separate mapping to the primitive
// field.
std::unique_ptr<const std::unordered_map<std::string, const FieldDescriptor*>>
MakeFieldMap(const Descriptor* descriptor) {
  auto field_map = absl::make_unique<
      std::unordered_map<std::string, const FieldDescriptor*>>();
  for (int i = 0; i < descriptor->field_count(); i++) {
    const FieldDescriptor* field = descriptor->field(i);
    if (IsChoiceType(field)) {
      std::unique_ptr<
          const std::unordered_map<std::string, const FieldDescriptor*>>
          inner_map = MakeFieldMap(field->message_type());
      for (auto iter = inner_map->begin(); iter != inner_map->end(); iter++) {
        std::string child_field_name = iter->first;
        if (child_field_name[0] == '_') {
          // Convert primitive extension field name to field on choice type,
          // e.g., value + _boolean -> _valueBoolean for Extension.value.
          child_field_name[1] = std::toupper(child_field_name[1]);
          (*field_map)[absl::StrCat("_", field->json_name(),
                                    child_field_name.substr(1))] = field;
        } else {
          // For non-primitive, just append them together as camelcase, e.g.,
          // value + boolean = valueBoolean
          child_field_name[0] = std::toupper(child_field_name[0]);
          (*field_map)[absl::StrCat(field->json_name(), child_field_name)] =
              field;
        }
      }
    } else {
      (*field_map)[field->json_name()] = field;
      if (field->type() == FieldDescriptor::TYPE_MESSAGE &&
          IsPrimitive(field->message_type())) {
        // Fhir JSON represents extensions to primitive fields as separate
        // standalone JSON objects, keyed by the "_" + field name.
        (*field_map)["_" + field->json_name()] = field;
      }
    }
  }
  return field_map;
}

// Gets a field map for a given descriptor.
// This memoizes the results of MakeFieldMap.
const std::unordered_map<std::string, const FieldDescriptor*>& GetFieldMap(
    const Descriptor* descriptor) {
  // Note that we memoize on descriptor address, since the values include
  // FieldDescriptor addresses, which will only be valid for a given address
  // of input descriptor
  static auto* memos =
      new absl::flat_hash_map<intptr_t,
                              std::unique_ptr<const std::unordered_map<
                                  std::string, const FieldDescriptor*>>>();
  static absl::Mutex memos_mutex(absl::kConstInit);

  const intptr_t memo_key = reinterpret_cast<intptr_t>(descriptor);

  memos_mutex.ReaderLock();
  const auto iter = memos->find(memo_key);
  if (iter != memos->end()) {
    memos_mutex.ReaderUnlock();
    return *iter->second;
  }
  memos_mutex.ReaderUnlock();

  absl::MutexLock lock(&memos_mutex);

  // Check if anything created and wrote the new entry while we were waiting
  // on the lock
  const auto inside_lock_iter = memos->find(memo_key);
  if (inside_lock_iter != memos->end()) {
    return *inside_lock_iter->second;
  }

  // There's still no memo, and we're holding the lock.  Write a new entry.
  (*memos)[memo_key] = MakeFieldMap(descriptor);
  return *(*memos)[memo_key];
}

typedef std::unordered_map<std::string, const FieldDescriptor*> FieldMap;

// Builds a map from ContainedResource field type to FieldDescriptor for that
// field.
std::unique_ptr<FieldMap> BuildResourceTypeMap(const Descriptor* descriptor) {
  auto map = absl::make_unique<FieldMap>();
  for (int i = 0; i < descriptor->field_count(); i++) {
    const FieldDescriptor* field = descriptor->field(i);
    (*map)[field->message_type()->name()] = field;
  }
  return map;
}

absl::StatusOr<const FieldDescriptor*> GetContainedResourceField(
    const Descriptor* contained_resource_desc,
    const std::string& resource_type) {
  static std::unordered_map<std::string, std::unique_ptr<FieldMap>>*
      field_table =
          new std::unordered_map<std::string, std::unique_ptr<FieldMap>>;

  const std::string& contained_resource_name =
      contained_resource_desc->full_name();
  auto field_table_iter = field_table->find(contained_resource_name);
  const FieldDescriptor* field;
  if (field_table_iter == field_table->end()) {
    auto field_map = BuildResourceTypeMap(contained_resource_desc);
    field = (*field_map)[resource_type];
    (*field_table)[contained_resource_name] = std::move(field_map);
  } else {
    field = (*field_table_iter->second)[resource_type];
  }
  if (!field) {
    return InvalidArgumentError(absl::StrCat(
        "No field on ", contained_resource_name, " with type ", resource_type));
  }
  return field;
}

class Parser {
 public:
  explicit Parser(const PrimitiveHandler* primitive_handler,
                  absl::TimeZone default_timezone,
                  const google::fhir::Parser::JsonSanitizer& sanitizer)
      : primitive_handler_(primitive_handler),
        default_timezone_(default_timezone),
        sanitizer_(sanitizer) {}

  absl::Status MergeMessage(const Json::Value& value, Message* target) const {
    const Descriptor* target_descriptor = target->GetDescriptor();
    // TODO: handle this with an annotation
    if (target_descriptor->name() == "ContainedResource") {
      return MergeContainedResource(value, target);
    }

    const std::unordered_map<std::string, const FieldDescriptor*>& field_map =
        GetFieldMap(target_descriptor);

    for (auto sub_value_iter = value.begin(); sub_value_iter != value.end();
         ++sub_value_iter) {
      const auto& field_entry = field_map.find(sub_value_iter.key().asString());
      if (field_entry != field_map.end()) {
        if (IsChoiceType(field_entry->second)) {
          FHIR_RETURN_IF_ERROR(MergeChoiceField(*sub_value_iter,
                                                field_entry->second,
                                                field_entry->first, target));
        } else {
          FHIR_RETURN_IF_ERROR(
              MergeField(*sub_value_iter, field_entry->second, target));
        }
      } else if (sub_value_iter.key().asString() == "resourceType") {
        std::string resource_type = sub_value_iter->asString();
        if (!IsResource(target_descriptor) ||
            target_descriptor->name() != resource_type) {
          return InvalidArgumentError(absl::StrCat(
              "Error merging json resource of type ", resource_type,
              " into message of type", target_descriptor->name()));
        }
      } else {
        if (sub_value_iter.key().asString() == "fhir_comments") {
          // fhir_comments can exist in a valid FHIR json, however,
          // it is not supported in the current FHIR protos.
          // Hence, we simply ignore it.
          continue;
        }

        return InvalidArgumentError(absl::StrCat(
            "Unable to merge field ", sub_value_iter.key().asString(),
            " into resource of type ", target_descriptor->full_name()));
      }
    }
    return absl::OkStatus();
  }

  absl::Status MergeContainedResource(const Json::Value& value,
                                      Message* target) const {
    // We handle contained resources in a special way, because despite
    // internally being a Oneof, it is not acually a choice-type in FHIR. The
    // JSON field name is just "resource", which doesn't give us any clues
    // about which field in the Oneof to set.  Instead, we need to inspect
    // the JSON input to determine its type.  Then, merge into that specific
    // field in the resource Oneof.
    std::string resource_type =
        value.get("resourceType", Json::Value::null).asString();
    FHIR_ASSIGN_OR_RETURN(
        const FieldDescriptor* contained_field,
        GetContainedResourceField(target->GetDescriptor(), resource_type));
    return MergeMessage(value, target->GetReflection()->MutableMessage(
                                   target, contained_field));
  }

  absl::Status MergeChoiceField(const Json::Value& json,
                                const FieldDescriptor* choice_field,
                                const std::string& field_name,
                                Message* parent) const {
    const Descriptor* choice_type_descriptor = choice_field->message_type();
    const auto& choice_type_field_map = GetFieldMap(choice_type_descriptor);
    std::string choice_field_name = field_name;
    if (field_name[0] == '_') {
      // E.g., _valueBoolean -> boolean
      choice_field_name =
          absl::StrCat("_" + choice_field_name.substr(
                                 1 + choice_field->json_name().length()));
      choice_field_name[1] = std::tolower(choice_field_name[1]);
    } else {
      // E.g., valueBoolean -> boolean
      choice_field_name =
          choice_field_name.substr(choice_field->json_name().length());
      choice_field_name[0] = std::tolower(choice_field_name[0]);
    }
    auto value_field_iter = choice_type_field_map.find(choice_field_name);
    if (value_field_iter == choice_type_field_map.end()) {
      return InvalidArgumentError(absl::StrCat(
          "Can't find ", choice_field_name, " on ", choice_field->full_name()));
    }
    Message* choice_msg =
        parent->GetReflection()->MutableMessage(parent, choice_field);
    return MergeField(json, value_field_iter->second, choice_msg);
  }

  // Given a JSON value, field, and parent message, merges the FHIR JSON into
  // the given field on the parent.
  // Note that we cannot just pass the field message, as this behaves
  // differently if the field has been previously set or not.
  absl::Status MergeField(const Json::Value& json, const FieldDescriptor* field,
                          Message* parent) const {
    const Reflection* parent_reflection = parent->GetReflection();
    // If the field is non-primitive make sure it hasn't been set yet.
    // Note that we allow primitive types to be set already, because FHIR
    // represents extensions to primitives as separate, subsequent JSON
    // elements, with the field prepended by an underscore.  In #GetFieldMap
    // above, these were mapped to the same fields.
    if (!IsPrimitive(field->message_type())) {
      if (!(field->is_repeated() &&
            parent_reflection->FieldSize(*parent, field) == 0) &&
          !(!field->is_repeated() &&
            !parent_reflection->HasField(*parent, field))) {
        return InvalidArgumentError(
            absl::StrCat("Target field already set: ", field->full_name()));
      }
    }

    if (field->containing_oneof()) {
      const ::google::protobuf::FieldDescriptor* oneof_field =
          parent_reflection->GetOneofFieldDescriptor(*parent,
                                                     field->containing_oneof());
      // Consider it an error to try to set a field in a oneof if one is already
      // set.
      // Exception: When a primitive in a choice type has a value and an
      // extension, it will get set twice, once by the value (e.g.,
      // valueString), and once by an extension (e.g., _valueString).
      if (oneof_field && !(IsPrimitive(field->message_type()) &&
                           oneof_field->full_name() == field->full_name())) {
        return InvalidArgumentError(absl::StrCat(
            "Cannot set field ", field->full_name(), " because another field ",
            oneof_field->full_name(), " of the same oneof is already set."));
      }
    }

    if (field->is_repeated()) {
      if (!json.isArray()) {
        return InvalidArgumentError(
            absl::StrCat("Attempted to set repeated field ", field->full_name(),
                         " using non-array JSON: ", json.toStyledString()));
      }
      size_t existing_field_size = parent_reflection->FieldSize(*parent, field);
      if (existing_field_size != 0 && existing_field_size != json.size()) {
        return InvalidArgumentError(absl::StrCat(
            "Repeated primitive list length does not match extension list ",
            "for field: ", field->full_name()));
      }
      for (Json::ArrayIndex i = 0; i < json.size(); i++) {
        FHIR_ASSIGN_OR_RETURN(
            std::unique_ptr<Message> parsed_value,
            ParseFieldValue(field, json.get(i, Json::Value::null), parent));
        if (existing_field_size > 0) {
          FHIR_RETURN_IF_ERROR(MergeAndClearPrimitiveWithNoValue(
              *parsed_value,
              parent_reflection->MutableRepeatedMessage(parent, field, i)));
        } else {
          parent_reflection->AddAllocatedMessage(parent, field,
                                                 parsed_value.release());
        }
      }
    } else {
      FHIR_ASSIGN_OR_RETURN(std::unique_ptr<Message> parsed_value,
                            ParseFieldValue(field, json, parent));
      if (parent_reflection->HasField(*parent, field)) {
        FHIR_RETURN_IF_ERROR(MergeAndClearPrimitiveWithNoValue(
            *parsed_value, parent_reflection->MutableMessage(parent, field)));
      } else {
        parent_reflection->SetAllocatedMessage(parent, parsed_value.release(),
                                               field);
      }
    }
    return absl::OkStatus();
  }

  absl::Status AddPrimitiveHasNoValueExtension(Message* message) const {
    Message* extension = message->GetReflection()->AddMessage(
        message, message->GetDescriptor()->FindFieldByName("extension"));
    return BuildHasNoValueExtension(extension);
  }

  absl::Status MergeAndClearPrimitiveWithNoValue(
      const Message& parsed_value, Message* field_to_modify) const {
    field_to_modify->MergeFrom(parsed_value);
    // This is the second time we've visited this field - once for
    // extensions, and once for value.  So, make sure to clear the
    // PrimitiveHasNoValue extension.
    return extensions_lib::ClearExtensionsWithUrl(
        primitives_internal::kPrimitiveHasNoValueUrl, field_to_modify);
  }

  absl::StatusOr<std::unique_ptr<Message>> ParseFieldValue(
      const FieldDescriptor* field, const Json::Value& json,
      Message* parent) const {
    if (field->type() != FieldDescriptor::Type::TYPE_MESSAGE) {
      return InvalidArgumentError(
          absl::StrCat("Error in FHIR proto definition: Field ",
                       field->full_name(), " is not a message."));
    }
    if (field->message_type()->full_name() == Any::descriptor()->full_name()) {
      std::unique_ptr<Message> contained =
          absl::WrapUnique(primitive_handler_->NewContainedResource());
      FHIR_RETURN_IF_ERROR(MergeContainedResource(json, contained.get()));
      Any* any = new Any;
      any->PackFrom(*contained);
      return absl::WrapUnique<Message>(any);
    } else {
      std::unique_ptr<Message> target =
          absl::WrapUnique(parent->GetReflection()
                               ->GetMessageFactory()
                               ->GetPrototype(field->message_type())
                               ->New());
      absl::Status merge_status;

      // Sanitize JSON value depending on the type of JSON value.
      if (json.isString()) {
        std::string val = json.asString();
        FHIR_RETURN_IF_ERROR(sanitizer_.SanitizeStringField(field, val));
        merge_status = MergeValue(Json::Value(val), target.get());
      } else {
        merge_status = MergeValue(json, target.get());
      }
      if (!merge_status.ok()) {
        return InvalidArgumentError(absl::StrCat("Error parsing field ",
                                                 field->json_name(), ": ",
                                                 merge_status.message()));
      }
      return std::move(target);
    }
  }

  absl::Status MergeValue(const Json::Value& json, Message* target) const {
    if (IsPrimitive(target->GetDescriptor())) {
      if (json.isObject()) {
        // This is a primitive type extension.
        // Merge the extension fields into into the empty target proto,
        // and tag it as having no value.
        FHIR_RETURN_IF_ERROR(MergeMessage(json, target));
        return BuildHasNoValueExtension(target->GetReflection()->AddMessage(
            target, target->GetDescriptor()->FindFieldByName("extension")));
      } else {
        return primitive_handler_->ParseInto(json, default_timezone_, target);
      }
    } else if (IsReference(target->GetDescriptor())) {
      FHIR_RETURN_IF_ERROR(MergeMessage(json, target));
      return SplitIfRelativeReference(target);
    }
    // Must be another FHIR element.
    if (!json.isObject()) {
      if (json.isArray() && json.size() == 1) {
        // The target field is non-repeated, and we're trying to populate it
        // with a single element array.
        // This is considered valid, and occurs when a profiled resource reduces
        // the size of a repeated FHIR field to max of 1.
        return MergeMessage(json.get(0u, Json::Value::null), target);
      }
      return InvalidArgumentError(
          absl::StrCat("Expected JsonObject for field of type ",
                       target->GetDescriptor()->full_name()));
    }
    return MergeMessage(json, target);
  }

 private:
  const PrimitiveHandler* primitive_handler_;
  const absl::TimeZone default_timezone_;
  const google::fhir::Parser::JsonSanitizer& sanitizer_;
};

absl::StatusOr<Json::Value> ParseJsonValue(const std::string& raw_json) {
  Json::Reader reader;
  Json::Value value;
  if (!reader.parse(raw_json, value)) {
    return InvalidArgumentError("Failed parsing raw json.");
  }
  return value;
}

}  // namespace internal

namespace {

static const char* const kJsonKeyRegex =
    R"("[^\s"]*[^\\]")";  // Quote, non-whitespace, unescaped quote

static const char* const kColonRegex =
    R"(\s*:\s*)";  // Arbitary whitespace, colon, arbitrary white space

static const char* const kDecimalRegex =
    "-?\\d*\\.\\d*?"                      // Decimal of the form X.X
    "|"                                   // OR
    "-?\\d*(?:\\.\\d*)?[eE][+-]?[0-9]+";  // Scientific Notation

static const char* const kEndTokenRegex = "[\\s,\\}\\]$]";

// Makes a regex to capture Key/Values of Decimal type.
// This regex contains 4 capture groups:
// 1: The key, identified a Quote, followed by non-whitespace,
// followed by an unescaped quote.
// 2: Arbitrary whitespace, colon, arbitrary whitespace
// 3: a decimal (either a number with a decimal, or scientific notation)
// 4: any field-ending token.
// Note that this does NOT match integers, so that only Decimal datatypes
// are affected by this particular eccentricity.
// Note that (1) is resiliant to the case of a string like
// { "myString": "stuff\"myFakeField\": 123" }
// because the end quote cannot be preceded by a backslash.
const LazyRE2& GetDecimalRegex() {
  const std::string re =
      absl::StrFormat("(?m)(%s)(%s)(%s)(%s)", kJsonKeyRegex, kColonRegex,
                      kDecimalRegex, kEndTokenRegex);
  static LazyRE2 regex{re.c_str()};
  // Dereference to cause init to run.
  // This allows the string memory to be released, since LazyRE2 starts off by
  // only holding on to the char* address.
  *regex;
  return regex;
}

}  // namespace

absl::Status Parser::MergeJsonFhirStringIntoProto(
    const std::string& raw_json, Message* target,
    const absl::TimeZone default_timezone, const bool validate) const {
  return MergeJsonFhirStringIntoProto(raw_json, target, default_timezone,
                                      PassThroughSanitizer(), validate);
}

absl::Status Parser::MergeJsonFhirStringIntoProto(
    const std::string& raw_json, Message* target,
    const absl::TimeZone default_timezone, const JsonSanitizer& sanitizer,
    const bool validate) const {
  std::string mutable_raw_json = raw_json;
  // FHIR JSON format stores decimals as unquoted rational numbers.  This is
  // problematic, because their representation could change when they are
  // parsed into C++ doubles.  To avoid this, add quotes around any decimal
  // fields to ensure that they are parsed as strings.  Note that any field
  // that is already in quotes will not match this regex, and thus be ignored.
  static const LazyRE2& decimal_regex = GetDecimalRegex();
  RE2::GlobalReplace(&mutable_raw_json, *decimal_regex, R"(\1\2"\3"\4)");

  Json::Value value;

  // TODO: Decide if we want to support value-only JSON
  if (IsDecimal(*target) && raw_json != "null") {
    // Similar to above, if this is a standalone decimal, parse it as a string
    // to avoid changing representation due to precision.
    FHIR_ASSIGN_OR_RETURN(value, internal::ParseJsonValue(absl::StrCat(
                                     "\"", mutable_raw_json, "\"")));
  } else {
    FHIR_ASSIGN_OR_RETURN(value, internal::ParseJsonValue(mutable_raw_json));
  }

  internal::Parser parser{primitive_handler_, default_timezone, sanitizer};

  // If the target is a profiled resource, first parse to the base resource,
  // and then profile to the target type.
  // Note that we do not do this for primitive profiled types like Code,
  // since those are handled directly by the primitive wrappers.
  if (IsProfile(target->GetDescriptor()) &&
      !IsPrimitive(target->GetDescriptor())) {
    FHIR_ASSIGN_OR_RETURN(std::unique_ptr<Message> core_resource,
                          GetBaseResourceInstance(*target));

    FHIR_RETURN_IF_ERROR(parser.MergeValue(value, core_resource.get()));

    // TODO: This is not ideal because it pulls in both stu3 and
    // r4 datatypes.
    switch (GetFhirVersion(*target)) {
      case proto::STU3:
        return validate ? ConvertToProfileStu3(*core_resource, target)
                        : ConvertToProfileLenientStu3(*core_resource, target);
      case proto::R4:
        return validate ? ConvertToProfileR4(*core_resource, target)
                        : ConvertToProfileLenientR4(*core_resource, target);
      default:
        return InvalidArgumentError(
            "Unsupported FHIR Version for profiling for resource: " +
            target->GetDescriptor()->full_name());
    }
  }

  FHIR_RETURN_IF_ERROR(parser.MergeValue(value, target));

  if (validate) {
    return ValidateResource(*target, primitive_handler_);
  }
  return absl::OkStatus();
}

}  // namespace fhir
}  // namespace google

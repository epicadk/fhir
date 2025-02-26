#
# Copyright 2020 Google LLC
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
"""Tests the primitive_handler module."""

import os

from absl.testing import absltest
from google.fhir import primitive_handler_test
from google.fhir.r4 import primitive_handler

_PRIMITIVE_HANDLER = primitive_handler.PrimitiveHandler()
_VALIDATION_DIR = os.path.join('testdata', 'r4', 'validation')


class PrimitiveWrapperPrimitiveHasNoValueTest(
    primitive_handler_test.PrimitiveWrapperPrimitiveHasNoValueTest):

  @property
  def primitive_handler(self) -> primitive_handler.PrimitiveHandler:
    return _PRIMITIVE_HANDLER

  def testPrimitiveHasNoValue_withValidCanoncial_succeeds(self):
    self.assert_set_valid_primitive_has_no_value_succeeds(
        self.primitive_handler.canonical_cls)

  def testPrimitiveHasNoValue_withInvalidCanonical_raises(self):
    self.assert_set_invalid_primitive_has_no_value_raises(
        self.primitive_handler.canonical_cls)

  def testPrimitiveHasNoValue_withInvalidUri_raises(self):
    # Note that URI had no REGEX validation pattern for STU3.
    # See more at: https://www.hl7.org/fhir/STU3/datatypes.html#uri.
    self.assert_set_invalid_primitive_has_no_value_raises(
        self.primitive_handler.uri_cls)

  def testPrimitiveHasNoValue_withValidUrl_succeeds(self):
    self.assert_set_valid_primitive_has_no_value_succeeds(
        self.primitive_handler.url_cls)

  def testPrimitiveHasNoValue_withInvalidUrl_raises(self):
    self.assert_set_invalid_primitive_has_no_value_raises(
        self.primitive_handler.url_cls)

  def testPrimitiveHasNoValue_withValidUuid_succeeds(self):
    self.assert_set_valid_primitive_has_no_value_succeeds(
        self.primitive_handler.uuid_cls)

  def testPrimitiveHasNoValue_withInvalidUuid_raises(self):
    self.assert_set_invalid_primitive_has_no_value_raises(
        self.primitive_handler.uuid_cls)


class PrimitiveWrapperProtoValidationTest(
    primitive_handler_test.PrimitiveWrapperProtoValidationTest):

  @property
  def primitive_handler(self) -> primitive_handler.PrimitiveHandler:
    return _PRIMITIVE_HANDLER

  @property
  def validation_dir(self) -> str:
    return _VALIDATION_DIR

  def testValidateWrapped_withValidCanonical_succeeds(self):
    self.assert_validation_of_valid_primitive_succeeds(
        self.primitive_handler.canonical_cls)

  def testValidateWrapped_withInvalidCanonical_raises(self):
    self.assert_validation_of_invalid_primitive_raises(
        self.primitive_handler.canonical_cls)

  def testValidateWrapped_withInvalidUri_raises(self):
    # Note that URI had no REGEX validation pattern for STU3.
    # See more at: https://www.hl7.org/fhir/STU3/datatypes.html#uri.
    self.assert_validation_of_invalid_primitive_raises(
        self.primitive_handler.uri_cls)

  def testValidateWrapped_withValidUrl_succeeds(self):
    self.assert_validation_of_valid_primitive_succeeds(
        self.primitive_handler.url_cls)

  def testValidateWrapped_withInvalidUrl_raises(self):
    self.assert_validation_of_invalid_primitive_raises(
        self.primitive_handler.url_cls)

  def testValidateWrapped_withValidUuid_succeeds(self):
    self.assert_validation_of_valid_primitive_succeeds(
        self.primitive_handler.uuid_cls)

  def testValidateWrapped_withInvalidUuid_raises(self):
    self.assert_validation_of_invalid_primitive_raises(
        self.primitive_handler.uuid_cls)


class PrimitiveWrapperJsonValidationTest(
    primitive_handler_test.PrimitiveWrapperJsonValidationTest):

  @property
  def primitive_handler(self) -> primitive_handler.PrimitiveHandler:
    return _PRIMITIVE_HANDLER

  @property
  def validation_dir(self) -> str:
    return _VALIDATION_DIR

  def testValidateWrapped_withValidCanonical_succeeds(self):
    self.assert_json_validation_with_valid_primitive_succeeds(
        self.primitive_handler.canonical_cls)

  def testValidateWrapped_withInvalidCanonical_raises(self):
    self.assert_json_validation_with_invalid_primitive_raises(
        self.primitive_handler.canonical_cls)

  def testValidateWrapped_withValidUrl_succeeds(self):
    self.assert_json_validation_with_valid_primitive_succeeds(
        self.primitive_handler.url_cls)

  def testValidateWrapped_withInvalidUrl_raises(self):
    self.assert_json_validation_with_invalid_primitive_raises(
        self.primitive_handler.url_cls)

  def testValidateWrapped_withValidUuid_succeeds(self):
    self.assert_json_validation_with_valid_primitive_succeeds(
        self.primitive_handler.uuid_cls)

  def testValidateWrapped_withInvalidUuid_raises(self):
    self.assert_json_validation_with_invalid_primitive_raises(
        self.primitive_handler.uuid_cls)


class DateTimeWrapperTest(primitive_handler_test.DateTimeWrapperTest):

  @property
  def primitive_handler(self) -> primitive_handler.PrimitiveHandler:
    return _PRIMITIVE_HANDLER


class DateWrapperTest(primitive_handler_test.DateWrapperTest):

  @property
  def primitive_handler(self) -> primitive_handler.PrimitiveHandler:
    return _PRIMITIVE_HANDLER


class InstantWrapperTest(primitive_handler_test.InstantWrapperTest):

  @property
  def primitive_handler(self) -> primitive_handler.PrimitiveHandler:
    return _PRIMITIVE_HANDLER


class TimeWrapperTest(primitive_handler_test.TimeWrapperTest):

  @property
  def primitive_handler(self) -> primitive_handler.PrimitiveHandler:
    return _PRIMITIVE_HANDLER


class DecimalWrapperTest(primitive_handler_test.DecimalWrapperTest):

  @property
  def primitive_handler(self) -> primitive_handler.PrimitiveHandler:
    return _PRIMITIVE_HANDLER


if __name__ == '__main__':
  absltest.main()

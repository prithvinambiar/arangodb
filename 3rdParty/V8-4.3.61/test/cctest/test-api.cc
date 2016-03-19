// Copyright 2012 the V8 project authors. All rights reserved.
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
//       copyright notice, this list of conditions and the following
//       disclaimer in the documentation and/or other materials provided
//       with the distribution.
//     * Neither the name of Google Inc. nor the names of its
//       contributors may be used to endorse or promote products derived
//       from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include <climits>
#include <csignal>
#include <map>
#include <string>

#include "test/cctest/test-api.h"

#if V8_OS_POSIX
#include <unistd.h>  // NOLINT
#endif

#include "include/v8-util.h"
#include "src/api.h"
#include "src/arguments.h"
#include "src/base/platform/platform.h"
#include "src/compilation-cache.h"
#include "src/execution.h"
#include "src/objects.h"
#include "src/parser.h"
#include "src/smart-pointers.h"
#include "src/unicode-inl.h"
#include "src/utils.h"
#include "src/vm-state.h"

static const bool kLogThreading = false;

using ::v8::Boolean;
using ::v8::BooleanObject;
using ::v8::Context;
using ::v8::Extension;
using ::v8::Function;
using ::v8::FunctionTemplate;
using ::v8::Handle;
using ::v8::HandleScope;
using ::v8::Local;
using ::v8::Maybe;
using ::v8::Message;
using ::v8::MessageCallback;
using ::v8::Name;
using ::v8::None;
using ::v8::Object;
using ::v8::ObjectTemplate;
using ::v8::Persistent;
using ::v8::PropertyAttribute;
using ::v8::Script;
using ::v8::StackTrace;
using ::v8::String;
using ::v8::Symbol;
using ::v8::TryCatch;
using ::v8::Undefined;
using ::v8::UniqueId;
using ::v8::V8;
using ::v8::Value;


#define THREADED_PROFILED_TEST(Name)                                 \
  static void Test##Name();                                          \
  TEST(Name##WithProfiler) {                                         \
    RunWithProfiler(&Test##Name);                                    \
  }                                                                  \
  THREADED_TEST(Name)


void RunWithProfiler(void (*test)()) {
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());
  v8::Local<v8::String> profile_name =
      v8::String::NewFromUtf8(env->GetIsolate(), "my_profile1");
  v8::CpuProfiler* cpu_profiler = env->GetIsolate()->GetCpuProfiler();

  cpu_profiler->StartProfiling(profile_name);
  (*test)();
  reinterpret_cast<i::CpuProfiler*>(cpu_profiler)->DeleteAllProfiles();
}


static int signature_callback_count;
static Local<Value> signature_expected_receiver;
static void IncrementingSignatureCallback(
    const v8::FunctionCallbackInfo<v8::Value>& args) {
  ApiTestFuzzer::Fuzz();
  signature_callback_count++;
  CHECK(signature_expected_receiver->Equals(args.Holder()));
  CHECK(signature_expected_receiver->Equals(args.This()));
  v8::Handle<v8::Array> result =
      v8::Array::New(args.GetIsolate(), args.Length());
  for (int i = 0; i < args.Length(); i++)
    result->Set(v8::Integer::New(args.GetIsolate(), i), args[i]);
  args.GetReturnValue().Set(result);
}


static void Returns42(const v8::FunctionCallbackInfo<v8::Value>& info) {
  info.GetReturnValue().Set(42);
}


// Tests that call v8::V8::Dispose() cannot be threaded.
UNINITIALIZED_TEST(InitializeAndDisposeOnce) {
  CHECK(v8::V8::Initialize());
  CHECK(v8::V8::Dispose());
}


// Tests that call v8::V8::Dispose() cannot be threaded.
UNINITIALIZED_TEST(InitializeAndDisposeMultiple) {
  for (int i = 0; i < 3; ++i) CHECK(v8::V8::Dispose());
  for (int i = 0; i < 3; ++i) CHECK(v8::V8::Initialize());
  for (int i = 0; i < 3; ++i) CHECK(v8::V8::Dispose());
  for (int i = 0; i < 3; ++i) CHECK(v8::V8::Initialize());
  for (int i = 0; i < 3; ++i) CHECK(v8::V8::Dispose());
}


THREADED_TEST(Handles) {
  v8::HandleScope scope(CcTest::isolate());
  Local<Context> local_env;
  {
    LocalContext env;
    local_env = env.local();
  }

  // Local context should still be live.
  CHECK(!local_env.IsEmpty());
  local_env->Enter();

  v8::Handle<v8::Primitive> undef = v8::Undefined(CcTest::isolate());
  CHECK(!undef.IsEmpty());
  CHECK(undef->IsUndefined());

  const char* source = "1 + 2 + 3";
  Local<Script> script = v8_compile(source);
  CHECK_EQ(6, script->Run()->Int32Value());

  local_env->Exit();
}


THREADED_TEST(IsolateOfContext) {
  v8::HandleScope scope(CcTest::isolate());
  v8::Handle<Context> env = Context::New(CcTest::isolate());

  CHECK(!env->GetIsolate()->InContext());
  CHECK(env->GetIsolate() == CcTest::isolate());
  env->Enter();
  CHECK(env->GetIsolate()->InContext());
  CHECK(env->GetIsolate() == CcTest::isolate());
  env->Exit();
  CHECK(!env->GetIsolate()->InContext());
  CHECK(env->GetIsolate() == CcTest::isolate());
}


static void TestSignature(const char* loop_js, Local<Value> receiver,
                          v8::Isolate* isolate) {
  i::ScopedVector<char> source(200);
  i::SNPrintF(source,
              "for (var i = 0; i < 10; i++) {"
              "  %s"
              "}",
              loop_js);
  signature_callback_count = 0;
  signature_expected_receiver = receiver;
  bool expected_to_throw = receiver.IsEmpty();
  v8::TryCatch try_catch;
  CompileRun(source.start());
  CHECK_EQ(expected_to_throw, try_catch.HasCaught());
  if (!expected_to_throw) {
    CHECK_EQ(10, signature_callback_count);
  } else {
    CHECK(v8_str("TypeError: Illegal invocation")
              ->Equals(try_catch.Exception()->ToString(isolate)));
  }
}


THREADED_TEST(ReceiverSignature) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);
  // Setup templates.
  v8::Handle<v8::FunctionTemplate> fun = v8::FunctionTemplate::New(isolate);
  v8::Handle<v8::Signature> sig = v8::Signature::New(isolate, fun);
  v8::Handle<v8::FunctionTemplate> callback_sig =
      v8::FunctionTemplate::New(
          isolate, IncrementingSignatureCallback, Local<Value>(), sig);
  v8::Handle<v8::FunctionTemplate> callback =
      v8::FunctionTemplate::New(isolate, IncrementingSignatureCallback);
  v8::Handle<v8::FunctionTemplate> sub_fun = v8::FunctionTemplate::New(isolate);
  sub_fun->Inherit(fun);
  v8::Handle<v8::FunctionTemplate> unrel_fun =
      v8::FunctionTemplate::New(isolate);
  // Install properties.
  v8::Handle<v8::ObjectTemplate> fun_proto = fun->PrototypeTemplate();
  fun_proto->Set(v8_str("prop_sig"), callback_sig);
  fun_proto->Set(v8_str("prop"), callback);
  fun_proto->SetAccessorProperty(
      v8_str("accessor_sig"), callback_sig, callback_sig);
  fun_proto->SetAccessorProperty(v8_str("accessor"), callback, callback);
  // Instantiate templates.
  Local<Value> fun_instance = fun->InstanceTemplate()->NewInstance();
  Local<Value> sub_fun_instance = sub_fun->InstanceTemplate()->NewInstance();
  // Setup global variables.
  env->Global()->Set(v8_str("Fun"), fun->GetFunction());
  env->Global()->Set(v8_str("UnrelFun"), unrel_fun->GetFunction());
  env->Global()->Set(v8_str("fun_instance"), fun_instance);
  env->Global()->Set(v8_str("sub_fun_instance"), sub_fun_instance);
  CompileRun(
      "var accessor_sig_key = 'accessor_sig';"
      "var accessor_key = 'accessor';"
      "var prop_sig_key = 'prop_sig';"
      "var prop_key = 'prop';"
      ""
      "function copy_props(obj) {"
      "  var keys = [accessor_sig_key, accessor_key, prop_sig_key, prop_key];"
      "  var source = Fun.prototype;"
      "  for (var i in keys) {"
      "    var key = keys[i];"
      "    var desc = Object.getOwnPropertyDescriptor(source, key);"
      "    Object.defineProperty(obj, key, desc);"
      "  }"
      "}"
      ""
      "var obj = {};"
      "copy_props(obj);"
      "var unrel = new UnrelFun();"
      "copy_props(unrel);");
  // Test with and without ICs
  const char* test_objects[] = {
      "fun_instance", "sub_fun_instance", "obj", "unrel" };
  unsigned bad_signature_start_offset = 2;
  for (unsigned i = 0; i < arraysize(test_objects); i++) {
    i::ScopedVector<char> source(200);
    i::SNPrintF(
        source, "var test_object = %s; test_object", test_objects[i]);
    Local<Value> test_object = CompileRun(source.start());
    TestSignature("test_object.prop();", test_object, isolate);
    TestSignature("test_object.accessor;", test_object, isolate);
    TestSignature("test_object[accessor_key];", test_object, isolate);
    TestSignature("test_object.accessor = 1;", test_object, isolate);
    TestSignature("test_object[accessor_key] = 1;", test_object, isolate);
    if (i >= bad_signature_start_offset) test_object = Local<Value>();
    TestSignature("test_object.prop_sig();", test_object, isolate);
    TestSignature("test_object.accessor_sig;", test_object, isolate);
    TestSignature("test_object[accessor_sig_key];", test_object, isolate);
    TestSignature("test_object.accessor_sig = 1;", test_object, isolate);
    TestSignature("test_object[accessor_sig_key] = 1;", test_object, isolate);
  }
}


THREADED_TEST(HulIgennem) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);
  v8::Handle<v8::Primitive> undef = v8::Undefined(isolate);
  Local<String> undef_str = undef->ToString(isolate);
  char* value = i::NewArray<char>(undef_str->Utf8Length() + 1);
  undef_str->WriteUtf8(value);
  CHECK_EQ(0, strcmp(value, "undefined"));
  i::DeleteArray(value);
}


THREADED_TEST(Access) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);
  Local<v8::Object> obj = v8::Object::New(isolate);
  Local<Value> foo_before = obj->Get(v8_str("foo"));
  CHECK(foo_before->IsUndefined());
  Local<String> bar_str = v8_str("bar");
  obj->Set(v8_str("foo"), bar_str);
  Local<Value> foo_after = obj->Get(v8_str("foo"));
  CHECK(!foo_after->IsUndefined());
  CHECK(foo_after->IsString());
  CHECK(bar_str->Equals(foo_after));
}


THREADED_TEST(AccessElement) {
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());
  Local<v8::Object> obj = v8::Object::New(env->GetIsolate());
  Local<Value> before = obj->Get(1);
  CHECK(before->IsUndefined());
  Local<String> bar_str = v8_str("bar");
  obj->Set(1, bar_str);
  Local<Value> after = obj->Get(1);
  CHECK(!after->IsUndefined());
  CHECK(after->IsString());
  CHECK(bar_str->Equals(after));

  Local<v8::Array> value = CompileRun("[\"a\", \"b\"]").As<v8::Array>();
  CHECK(v8_str("a")->Equals(value->Get(0)));
  CHECK(v8_str("b")->Equals(value->Get(1)));
}


THREADED_TEST(Script) {
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());
  const char* source = "1 + 2 + 3";
  Local<Script> script = v8_compile(source);
  CHECK_EQ(6, script->Run()->Int32Value());
}


class TestResource: public String::ExternalStringResource {
 public:
  explicit TestResource(uint16_t* data, int* counter = NULL,
                        bool owning_data = true)
      : data_(data), length_(0), counter_(counter), owning_data_(owning_data) {
    while (data[length_]) ++length_;
  }

  ~TestResource() {
    if (owning_data_) i::DeleteArray(data_);
    if (counter_ != NULL) ++*counter_;
  }

  const uint16_t* data() const {
    return data_;
  }

  size_t length() const {
    return length_;
  }

 private:
  uint16_t* data_;
  size_t length_;
  int* counter_;
  bool owning_data_;
};


class TestOneByteResource : public String::ExternalOneByteStringResource {
 public:
  explicit TestOneByteResource(const char* data, int* counter = NULL,
                               size_t offset = 0)
      : orig_data_(data),
        data_(data + offset),
        length_(strlen(data) - offset),
        counter_(counter) {}

  ~TestOneByteResource() {
    i::DeleteArray(orig_data_);
    if (counter_ != NULL) ++*counter_;
  }

  const char* data() const {
    return data_;
  }

  size_t length() const {
    return length_;
  }

 private:
  const char* orig_data_;
  const char* data_;
  size_t length_;
  int* counter_;
};


THREADED_TEST(ScriptUsingStringResource) {
  int dispose_count = 0;
  const char* c_source = "1 + 2 * 3";
  uint16_t* two_byte_source = AsciiToTwoByteString(c_source);
  {
    LocalContext env;
    v8::HandleScope scope(env->GetIsolate());
    TestResource* resource = new TestResource(two_byte_source, &dispose_count);
    Local<String> source = String::NewExternal(env->GetIsolate(), resource);
    Local<Script> script = v8_compile(source);
    Local<Value> value = script->Run();
    CHECK(value->IsNumber());
    CHECK_EQ(7, value->Int32Value());
    CHECK(source->IsExternal());
    CHECK_EQ(resource,
             static_cast<TestResource*>(source->GetExternalStringResource()));
    String::Encoding encoding = String::UNKNOWN_ENCODING;
    CHECK_EQ(static_cast<const String::ExternalStringResourceBase*>(resource),
             source->GetExternalStringResourceBase(&encoding));
    CHECK_EQ(String::TWO_BYTE_ENCODING, encoding);
    CcTest::heap()->CollectAllGarbage(i::Heap::kNoGCFlags);
    CHECK_EQ(0, dispose_count);
  }
  CcTest::i_isolate()->compilation_cache()->Clear();
  CcTest::heap()->CollectAllAvailableGarbage();
  CHECK_EQ(1, dispose_count);
}


THREADED_TEST(ScriptUsingOneByteStringResource) {
  int dispose_count = 0;
  const char* c_source = "1 + 2 * 3";
  {
    LocalContext env;
    v8::HandleScope scope(env->GetIsolate());
    TestOneByteResource* resource =
        new TestOneByteResource(i::StrDup(c_source), &dispose_count);
    Local<String> source = String::NewExternal(env->GetIsolate(), resource);
    CHECK(source->IsExternalOneByte());
    CHECK_EQ(static_cast<const String::ExternalStringResourceBase*>(resource),
             source->GetExternalOneByteStringResource());
    String::Encoding encoding = String::UNKNOWN_ENCODING;
    CHECK_EQ(static_cast<const String::ExternalStringResourceBase*>(resource),
             source->GetExternalStringResourceBase(&encoding));
    CHECK_EQ(String::ONE_BYTE_ENCODING, encoding);
    Local<Script> script = v8_compile(source);
    Local<Value> value = script->Run();
    CHECK(value->IsNumber());
    CHECK_EQ(7, value->Int32Value());
    CcTest::heap()->CollectAllGarbage(i::Heap::kNoGCFlags);
    CHECK_EQ(0, dispose_count);
  }
  CcTest::i_isolate()->compilation_cache()->Clear();
  CcTest::heap()->CollectAllAvailableGarbage();
  CHECK_EQ(1, dispose_count);
}


THREADED_TEST(ScriptMakingExternalString) {
  int dispose_count = 0;
  uint16_t* two_byte_source = AsciiToTwoByteString("1 + 2 * 3");
  {
    LocalContext env;
    v8::HandleScope scope(env->GetIsolate());
    Local<String> source =
        String::NewFromTwoByte(env->GetIsolate(), two_byte_source);
    // Trigger GCs so that the newly allocated string moves to old gen.
    CcTest::heap()->CollectGarbage(i::NEW_SPACE);  // in survivor space now
    CcTest::heap()->CollectGarbage(i::NEW_SPACE);  // in old gen now
    CHECK_EQ(source->IsExternal(), false);
    CHECK_EQ(source->IsExternalOneByte(), false);
    String::Encoding encoding = String::UNKNOWN_ENCODING;
    CHECK(!source->GetExternalStringResourceBase(&encoding));
    CHECK_EQ(String::ONE_BYTE_ENCODING, encoding);
    bool success = source->MakeExternal(new TestResource(two_byte_source,
                                                         &dispose_count));
    CHECK(success);
    Local<Script> script = v8_compile(source);
    Local<Value> value = script->Run();
    CHECK(value->IsNumber());
    CHECK_EQ(7, value->Int32Value());
    CcTest::heap()->CollectAllGarbage(i::Heap::kNoGCFlags);
    CHECK_EQ(0, dispose_count);
  }
  CcTest::i_isolate()->compilation_cache()->Clear();
  CcTest::heap()->CollectAllGarbage(i::Heap::kAbortIncrementalMarkingMask);
  CHECK_EQ(1, dispose_count);
}


THREADED_TEST(ScriptMakingExternalOneByteString) {
  int dispose_count = 0;
  const char* c_source = "1 + 2 * 3";
  {
    LocalContext env;
    v8::HandleScope scope(env->GetIsolate());
    Local<String> source = v8_str(c_source);
    // Trigger GCs so that the newly allocated string moves to old gen.
    CcTest::heap()->CollectGarbage(i::NEW_SPACE);  // in survivor space now
    CcTest::heap()->CollectGarbage(i::NEW_SPACE);  // in old gen now
    bool success = source->MakeExternal(
        new TestOneByteResource(i::StrDup(c_source), &dispose_count));
    CHECK(success);
    Local<Script> script = v8_compile(source);
    Local<Value> value = script->Run();
    CHECK(value->IsNumber());
    CHECK_EQ(7, value->Int32Value());
    CcTest::heap()->CollectAllGarbage(i::Heap::kNoGCFlags);
    CHECK_EQ(0, dispose_count);
  }
  CcTest::i_isolate()->compilation_cache()->Clear();
  CcTest::heap()->CollectAllGarbage(i::Heap::kAbortIncrementalMarkingMask);
  CHECK_EQ(1, dispose_count);
}


TEST(MakingExternalStringConditions) {
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());

  // Free some space in the new space so that we can check freshness.
  CcTest::heap()->CollectGarbage(i::NEW_SPACE);
  CcTest::heap()->CollectGarbage(i::NEW_SPACE);

  uint16_t* two_byte_string = AsciiToTwoByteString("s1");
  Local<String> small_string =
      String::NewFromTwoByte(env->GetIsolate(), two_byte_string);
  i::DeleteArray(two_byte_string);

  // We should refuse to externalize newly created small string.
  CHECK(!small_string->CanMakeExternal());
  // Trigger GCs so that the newly allocated string moves to old gen.
  CcTest::heap()->CollectGarbage(i::NEW_SPACE);  // in survivor space now
  CcTest::heap()->CollectGarbage(i::NEW_SPACE);  // in old gen now
  // Old space strings should be accepted.
  CHECK(small_string->CanMakeExternal());

  two_byte_string = AsciiToTwoByteString("small string 2");
  small_string = String::NewFromTwoByte(env->GetIsolate(), two_byte_string);
  i::DeleteArray(two_byte_string);

  // We should refuse externalizing newly created small string.
  CHECK(!small_string->CanMakeExternal());
  for (int i = 0; i < 100; i++) {
    String::Value value(small_string);
  }
  // Frequently used strings should be accepted.
  CHECK(small_string->CanMakeExternal());

  const int buf_size = 10 * 1024;
  char* buf = i::NewArray<char>(buf_size);
  memset(buf, 'a', buf_size);
  buf[buf_size - 1] = '\0';

  two_byte_string = AsciiToTwoByteString(buf);
  Local<String> large_string =
      String::NewFromTwoByte(env->GetIsolate(), two_byte_string);
  i::DeleteArray(buf);
  i::DeleteArray(two_byte_string);
  // Large strings should be immediately accepted.
  CHECK(large_string->CanMakeExternal());
}


TEST(MakingExternalOneByteStringConditions) {
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());

  // Free some space in the new space so that we can check freshness.
  CcTest::heap()->CollectGarbage(i::NEW_SPACE);
  CcTest::heap()->CollectGarbage(i::NEW_SPACE);

  Local<String> small_string = String::NewFromUtf8(env->GetIsolate(), "s1");
  // We should refuse to externalize newly created small string.
  CHECK(!small_string->CanMakeExternal());
  // Trigger GCs so that the newly allocated string moves to old gen.
  CcTest::heap()->CollectGarbage(i::NEW_SPACE);  // in survivor space now
  CcTest::heap()->CollectGarbage(i::NEW_SPACE);  // in old gen now
  // Old space strings should be accepted.
  CHECK(small_string->CanMakeExternal());

  small_string = String::NewFromUtf8(env->GetIsolate(), "small string 2");
  // We should refuse externalizing newly created small string.
  CHECK(!small_string->CanMakeExternal());
  for (int i = 0; i < 100; i++) {
    String::Value value(small_string);
  }
  // Frequently used strings should be accepted.
  CHECK(small_string->CanMakeExternal());

  const int buf_size = 10 * 1024;
  char* buf = i::NewArray<char>(buf_size);
  memset(buf, 'a', buf_size);
  buf[buf_size - 1] = '\0';
  Local<String> large_string = String::NewFromUtf8(env->GetIsolate(), buf);
  i::DeleteArray(buf);
  // Large strings should be immediately accepted.
  CHECK(large_string->CanMakeExternal());
}


TEST(MakingExternalUnalignedOneByteString) {
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());

  CompileRun("function cons(a, b) { return a + b; }"
             "function slice(a) { return a.substring(1); }");
  // Create a cons string that will land in old pointer space.
  Local<String> cons = Local<String>::Cast(CompileRun(
      "cons('abcdefghijklm', 'nopqrstuvwxyz');"));
  // Create a sliced string that will land in old pointer space.
  Local<String> slice = Local<String>::Cast(CompileRun(
      "slice('abcdefghijklmnopqrstuvwxyz');"));

  // Trigger GCs so that the newly allocated string moves to old gen.
  SimulateFullSpace(CcTest::heap()->old_pointer_space());
  CcTest::heap()->CollectGarbage(i::NEW_SPACE);  // in survivor space now
  CcTest::heap()->CollectGarbage(i::NEW_SPACE);  // in old gen now

  // Turn into external string with unaligned resource data.
  const char* c_cons = "_abcdefghijklmnopqrstuvwxyz";
  bool success =
      cons->MakeExternal(new TestOneByteResource(i::StrDup(c_cons), NULL, 1));
  CHECK(success);
  const char* c_slice = "_bcdefghijklmnopqrstuvwxyz";
  success =
      slice->MakeExternal(new TestOneByteResource(i::StrDup(c_slice), NULL, 1));
  CHECK(success);

  // Trigger GCs and force evacuation.
  CcTest::heap()->CollectAllGarbage(i::Heap::kNoGCFlags);
  CcTest::heap()->CollectAllGarbage(i::Heap::kReduceMemoryFootprintMask);
}


THREADED_TEST(UsingExternalString) {
  i::Factory* factory = CcTest::i_isolate()->factory();
  {
    v8::HandleScope scope(CcTest::isolate());
    uint16_t* two_byte_string = AsciiToTwoByteString("test string");
    Local<String> string = String::NewExternal(
        CcTest::isolate(), new TestResource(two_byte_string));
    i::Handle<i::String> istring = v8::Utils::OpenHandle(*string);
    // Trigger GCs so that the newly allocated string moves to old gen.
    CcTest::heap()->CollectGarbage(i::NEW_SPACE);  // in survivor space now
    CcTest::heap()->CollectGarbage(i::NEW_SPACE);  // in old gen now
    i::Handle<i::String> isymbol =
        factory->InternalizeString(istring);
    CHECK(isymbol->IsInternalizedString());
  }
  CcTest::heap()->CollectAllGarbage(i::Heap::kNoGCFlags);
  CcTest::heap()->CollectAllGarbage(i::Heap::kNoGCFlags);
}


THREADED_TEST(UsingExternalOneByteString) {
  i::Factory* factory = CcTest::i_isolate()->factory();
  {
    v8::HandleScope scope(CcTest::isolate());
    const char* one_byte_string = "test string";
    Local<String> string = String::NewExternal(
        CcTest::isolate(), new TestOneByteResource(i::StrDup(one_byte_string)));
    i::Handle<i::String> istring = v8::Utils::OpenHandle(*string);
    // Trigger GCs so that the newly allocated string moves to old gen.
    CcTest::heap()->CollectGarbage(i::NEW_SPACE);  // in survivor space now
    CcTest::heap()->CollectGarbage(i::NEW_SPACE);  // in old gen now
    i::Handle<i::String> isymbol =
        factory->InternalizeString(istring);
    CHECK(isymbol->IsInternalizedString());
  }
  CcTest::heap()->CollectAllGarbage(i::Heap::kNoGCFlags);
  CcTest::heap()->CollectAllGarbage(i::Heap::kNoGCFlags);
}


class RandomLengthResource : public v8::String::ExternalStringResource {
 public:
  explicit RandomLengthResource(int length) : length_(length) {}
  virtual const uint16_t* data() const { return string_; }
  virtual size_t length() const { return length_; }

 private:
  uint16_t string_[10];
  int length_;
};


class RandomLengthOneByteResource
    : public v8::String::ExternalOneByteStringResource {
 public:
  explicit RandomLengthOneByteResource(int length) : length_(length) {}
  virtual const char* data() const { return string_; }
  virtual size_t length() const { return length_; }

 private:
  char string_[10];
  int length_;
};


THREADED_TEST(NewExternalForVeryLongString) {
  auto isolate = CcTest::isolate();
  {
    v8::HandleScope scope(isolate);
    v8::TryCatch try_catch;
    RandomLengthOneByteResource r(1 << 30);
    v8::Local<v8::String> str = v8::String::NewExternal(isolate, &r);
    CHECK(str.IsEmpty());
    CHECK(!try_catch.HasCaught());
  }

  {
    v8::HandleScope scope(isolate);
    v8::TryCatch try_catch;
    RandomLengthResource r(1 << 30);
    v8::Local<v8::String> str = v8::String::NewExternal(isolate, &r);
    CHECK(str.IsEmpty());
    CHECK(!try_catch.HasCaught());
  }
}


THREADED_TEST(ScavengeExternalString) {
  i::FLAG_stress_compaction = false;
  i::FLAG_gc_global = false;
  int dispose_count = 0;
  bool in_new_space = false;
  {
    v8::HandleScope scope(CcTest::isolate());
    uint16_t* two_byte_string = AsciiToTwoByteString("test string");
    Local<String> string = String::NewExternal(
        CcTest::isolate(), new TestResource(two_byte_string, &dispose_count));
    i::Handle<i::String> istring = v8::Utils::OpenHandle(*string);
    CcTest::heap()->CollectGarbage(i::NEW_SPACE);
    in_new_space = CcTest::heap()->InNewSpace(*istring);
    CHECK(in_new_space || CcTest::heap()->old_data_space()->Contains(*istring));
    CHECK_EQ(0, dispose_count);
  }
  CcTest::heap()->CollectGarbage(in_new_space ? i::NEW_SPACE
                                              : i::OLD_DATA_SPACE);
  CHECK_EQ(1, dispose_count);
}


THREADED_TEST(ScavengeExternalOneByteString) {
  i::FLAG_stress_compaction = false;
  i::FLAG_gc_global = false;
  int dispose_count = 0;
  bool in_new_space = false;
  {
    v8::HandleScope scope(CcTest::isolate());
    const char* one_byte_string = "test string";
    Local<String> string = String::NewExternal(
        CcTest::isolate(),
        new TestOneByteResource(i::StrDup(one_byte_string), &dispose_count));
    i::Handle<i::String> istring = v8::Utils::OpenHandle(*string);
    CcTest::heap()->CollectGarbage(i::NEW_SPACE);
    in_new_space = CcTest::heap()->InNewSpace(*istring);
    CHECK(in_new_space || CcTest::heap()->old_data_space()->Contains(*istring));
    CHECK_EQ(0, dispose_count);
  }
  CcTest::heap()->CollectGarbage(in_new_space ? i::NEW_SPACE
                                              : i::OLD_DATA_SPACE);
  CHECK_EQ(1, dispose_count);
}


class TestOneByteResourceWithDisposeControl : public TestOneByteResource {
 public:
  // Only used by non-threaded tests, so it can use static fields.
  static int dispose_calls;
  static int dispose_count;

  TestOneByteResourceWithDisposeControl(const char* data, bool dispose)
      : TestOneByteResource(data, &dispose_count), dispose_(dispose) {}

  void Dispose() {
    ++dispose_calls;
    if (dispose_) delete this;
  }
 private:
  bool dispose_;
};


int TestOneByteResourceWithDisposeControl::dispose_count = 0;
int TestOneByteResourceWithDisposeControl::dispose_calls = 0;


TEST(ExternalStringWithDisposeHandling) {
  const char* c_source = "1 + 2 * 3";

  // Use a stack allocated external string resource allocated object.
  TestOneByteResourceWithDisposeControl::dispose_count = 0;
  TestOneByteResourceWithDisposeControl::dispose_calls = 0;
  TestOneByteResourceWithDisposeControl res_stack(i::StrDup(c_source), false);
  {
    LocalContext env;
    v8::HandleScope scope(env->GetIsolate());
    Local<String> source =  String::NewExternal(env->GetIsolate(), &res_stack);
    Local<Script> script = v8_compile(source);
    Local<Value> value = script->Run();
    CHECK(value->IsNumber());
    CHECK_EQ(7, value->Int32Value());
    CcTest::heap()->CollectAllAvailableGarbage();
    CHECK_EQ(0, TestOneByteResourceWithDisposeControl::dispose_count);
  }
  CcTest::i_isolate()->compilation_cache()->Clear();
  CcTest::heap()->CollectAllAvailableGarbage();
  CHECK_EQ(1, TestOneByteResourceWithDisposeControl::dispose_calls);
  CHECK_EQ(0, TestOneByteResourceWithDisposeControl::dispose_count);

  // Use a heap allocated external string resource allocated object.
  TestOneByteResourceWithDisposeControl::dispose_count = 0;
  TestOneByteResourceWithDisposeControl::dispose_calls = 0;
  TestOneByteResource* res_heap =
      new TestOneByteResourceWithDisposeControl(i::StrDup(c_source), true);
  {
    LocalContext env;
    v8::HandleScope scope(env->GetIsolate());
    Local<String> source =  String::NewExternal(env->GetIsolate(), res_heap);
    Local<Script> script = v8_compile(source);
    Local<Value> value = script->Run();
    CHECK(value->IsNumber());
    CHECK_EQ(7, value->Int32Value());
    CcTest::heap()->CollectAllAvailableGarbage();
    CHECK_EQ(0, TestOneByteResourceWithDisposeControl::dispose_count);
  }
  CcTest::i_isolate()->compilation_cache()->Clear();
  CcTest::heap()->CollectAllAvailableGarbage();
  CHECK_EQ(1, TestOneByteResourceWithDisposeControl::dispose_calls);
  CHECK_EQ(1, TestOneByteResourceWithDisposeControl::dispose_count);
}


THREADED_TEST(StringConcat) {
  {
    LocalContext env;
    v8::HandleScope scope(env->GetIsolate());
    const char* one_byte_string_1 = "function a_times_t";
    const char* two_byte_string_1 = "wo_plus_b(a, b) {return ";
    const char* one_byte_extern_1 = "a * 2 + b;} a_times_two_plus_b(4, 8) + ";
    const char* two_byte_extern_1 = "a_times_two_plus_b(4, 8) + ";
    const char* one_byte_string_2 = "a_times_two_plus_b(4, 8) + ";
    const char* two_byte_string_2 = "a_times_two_plus_b(4, 8) + ";
    const char* two_byte_extern_2 = "a_times_two_plus_b(1, 2);";
    Local<String> left = v8_str(one_byte_string_1);

    uint16_t* two_byte_source = AsciiToTwoByteString(two_byte_string_1);
    Local<String> right =
        String::NewFromTwoByte(env->GetIsolate(), two_byte_source);
    i::DeleteArray(two_byte_source);

    Local<String> source = String::Concat(left, right);
    right = String::NewExternal(
        env->GetIsolate(),
        new TestOneByteResource(i::StrDup(one_byte_extern_1)));
    source = String::Concat(source, right);
    right = String::NewExternal(
        env->GetIsolate(),
        new TestResource(AsciiToTwoByteString(two_byte_extern_1)));
    source = String::Concat(source, right);
    right = v8_str(one_byte_string_2);
    source = String::Concat(source, right);

    two_byte_source = AsciiToTwoByteString(two_byte_string_2);
    right = String::NewFromTwoByte(env->GetIsolate(), two_byte_source);
    i::DeleteArray(two_byte_source);

    source = String::Concat(source, right);
    right = String::NewExternal(
        env->GetIsolate(),
        new TestResource(AsciiToTwoByteString(two_byte_extern_2)));
    source = String::Concat(source, right);
    Local<Script> script = v8_compile(source);
    Local<Value> value = script->Run();
    CHECK(value->IsNumber());
    CHECK_EQ(68, value->Int32Value());
  }
  CcTest::i_isolate()->compilation_cache()->Clear();
  CcTest::heap()->CollectAllGarbage(i::Heap::kNoGCFlags);
  CcTest::heap()->CollectAllGarbage(i::Heap::kNoGCFlags);
}


THREADED_TEST(GlobalProperties) {
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());
  v8::Handle<v8::Object> global = env->Global();
  global->Set(v8_str("pi"), v8_num(3.1415926));
  Local<Value> pi = global->Get(v8_str("pi"));
  CHECK_EQ(3.1415926, pi->NumberValue());
}


static void handle_callback_impl(const v8::FunctionCallbackInfo<Value>& info,
                                 i::Address callback) {
  ApiTestFuzzer::Fuzz();
  CheckReturnValue(info, callback);
  info.GetReturnValue().Set(v8_str("bad value"));
  info.GetReturnValue().Set(v8_num(102));
}


static void handle_callback(const v8::FunctionCallbackInfo<Value>& info) {
  return handle_callback_impl(info, FUNCTION_ADDR(handle_callback));
}


static void handle_callback_2(const v8::FunctionCallbackInfo<Value>& info) {
  return handle_callback_impl(info, FUNCTION_ADDR(handle_callback_2));
}

static void construct_callback(
    const v8::FunctionCallbackInfo<Value>& info) {
  ApiTestFuzzer::Fuzz();
  CheckReturnValue(info, FUNCTION_ADDR(construct_callback));
  info.This()->Set(v8_str("x"), v8_num(1));
  info.This()->Set(v8_str("y"), v8_num(2));
  info.GetReturnValue().Set(v8_str("bad value"));
  info.GetReturnValue().Set(info.This());
}


static void Return239Callback(
    Local<String> name, const v8::PropertyCallbackInfo<Value>& info) {
  ApiTestFuzzer::Fuzz();
  CheckReturnValue(info, FUNCTION_ADDR(Return239Callback));
  info.GetReturnValue().Set(v8_str("bad value"));
  info.GetReturnValue().Set(v8_num(239));
}


template<typename Handler>
static void TestFunctionTemplateInitializer(Handler handler,
                                            Handler handler_2) {
  // Test constructor calls.
  {
    LocalContext env;
    v8::Isolate* isolate = env->GetIsolate();
    v8::HandleScope scope(isolate);

    Local<v8::FunctionTemplate> fun_templ =
        v8::FunctionTemplate::New(isolate, handler);
    Local<Function> fun = fun_templ->GetFunction();
    env->Global()->Set(v8_str("obj"), fun);
    Local<Script> script = v8_compile("obj()");
    for (int i = 0; i < 30; i++) {
      CHECK_EQ(102, script->Run()->Int32Value());
    }
  }
  // Use SetCallHandler to initialize a function template, should work like
  // the previous one.
  {
    LocalContext env;
    v8::Isolate* isolate = env->GetIsolate();
    v8::HandleScope scope(isolate);

    Local<v8::FunctionTemplate> fun_templ = v8::FunctionTemplate::New(isolate);
    fun_templ->SetCallHandler(handler_2);
    Local<Function> fun = fun_templ->GetFunction();
    env->Global()->Set(v8_str("obj"), fun);
    Local<Script> script = v8_compile("obj()");
    for (int i = 0; i < 30; i++) {
      CHECK_EQ(102, script->Run()->Int32Value());
    }
  }
}


template<typename Constructor, typename Accessor>
static void TestFunctionTemplateAccessor(Constructor constructor,
                                         Accessor accessor) {
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());

  Local<v8::FunctionTemplate> fun_templ =
      v8::FunctionTemplate::New(env->GetIsolate(), constructor);
  fun_templ->SetClassName(v8_str("funky"));
  fun_templ->InstanceTemplate()->SetAccessor(v8_str("m"), accessor);
  Local<Function> fun = fun_templ->GetFunction();
  env->Global()->Set(v8_str("obj"), fun);
  Local<Value> result = v8_compile("(new obj()).toString()")->Run();
  CHECK(v8_str("[object funky]")->Equals(result));
  CompileRun("var obj_instance = new obj();");
  Local<Script> script;
  script = v8_compile("obj_instance.x");
  for (int i = 0; i < 30; i++) {
    CHECK_EQ(1, script->Run()->Int32Value());
  }
  script = v8_compile("obj_instance.m");
  for (int i = 0; i < 30; i++) {
    CHECK_EQ(239, script->Run()->Int32Value());
  }
}


THREADED_PROFILED_TEST(FunctionTemplate) {
  TestFunctionTemplateInitializer(handle_callback, handle_callback_2);
  TestFunctionTemplateAccessor(construct_callback, Return239Callback);
}


static void SimpleCallback(const v8::FunctionCallbackInfo<v8::Value>& info) {
  ApiTestFuzzer::Fuzz();
  CheckReturnValue(info, FUNCTION_ADDR(SimpleCallback));
  info.GetReturnValue().Set(v8_num(51423 + info.Length()));
}


template<typename Callback>
static void TestSimpleCallback(Callback callback) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);

  v8::Handle<v8::ObjectTemplate> object_template =
      v8::ObjectTemplate::New(isolate);
  object_template->Set(isolate, "callback",
                       v8::FunctionTemplate::New(isolate, callback));
  v8::Local<v8::Object> object = object_template->NewInstance();
  (*env)->Global()->Set(v8_str("callback_object"), object);
  v8::Handle<v8::Script> script;
  script = v8_compile("callback_object.callback(17)");
  for (int i = 0; i < 30; i++) {
    CHECK_EQ(51424, script->Run()->Int32Value());
  }
  script = v8_compile("callback_object.callback(17, 24)");
  for (int i = 0; i < 30; i++) {
    CHECK_EQ(51425, script->Run()->Int32Value());
  }
}


THREADED_PROFILED_TEST(SimpleCallback) {
  TestSimpleCallback(SimpleCallback);
}


template<typename T>
void FastReturnValueCallback(const v8::FunctionCallbackInfo<v8::Value>& info);

// constant return values
static int32_t fast_return_value_int32 = 471;
static uint32_t fast_return_value_uint32 = 571;
static const double kFastReturnValueDouble = 2.7;
// variable return values
static bool fast_return_value_bool = false;
enum ReturnValueOddball {
  kNullReturnValue,
  kUndefinedReturnValue,
  kEmptyStringReturnValue
};
static ReturnValueOddball fast_return_value_void;
static bool fast_return_value_object_is_empty = false;

// Helper function to avoid compiler error: insufficient contextual information
// to determine type when applying FUNCTION_ADDR to a template function.
static i::Address address_of(v8::FunctionCallback callback) {
  return FUNCTION_ADDR(callback);
}

template<>
void FastReturnValueCallback<int32_t>(
    const v8::FunctionCallbackInfo<v8::Value>& info) {
  CheckReturnValue(info, address_of(FastReturnValueCallback<int32_t>));
  info.GetReturnValue().Set(fast_return_value_int32);
}

template<>
void FastReturnValueCallback<uint32_t>(
    const v8::FunctionCallbackInfo<v8::Value>& info) {
  CheckReturnValue(info, address_of(FastReturnValueCallback<uint32_t>));
  info.GetReturnValue().Set(fast_return_value_uint32);
}

template<>
void FastReturnValueCallback<double>(
    const v8::FunctionCallbackInfo<v8::Value>& info) {
  CheckReturnValue(info, address_of(FastReturnValueCallback<double>));
  info.GetReturnValue().Set(kFastReturnValueDouble);
}

template<>
void FastReturnValueCallback<bool>(
    const v8::FunctionCallbackInfo<v8::Value>& info) {
  CheckReturnValue(info, address_of(FastReturnValueCallback<bool>));
  info.GetReturnValue().Set(fast_return_value_bool);
}

template<>
void FastReturnValueCallback<void>(
    const v8::FunctionCallbackInfo<v8::Value>& info) {
  CheckReturnValue(info, address_of(FastReturnValueCallback<void>));
  switch (fast_return_value_void) {
    case kNullReturnValue:
      info.GetReturnValue().SetNull();
      break;
    case kUndefinedReturnValue:
      info.GetReturnValue().SetUndefined();
      break;
    case kEmptyStringReturnValue:
      info.GetReturnValue().SetEmptyString();
      break;
  }
}

template<>
void FastReturnValueCallback<Object>(
    const v8::FunctionCallbackInfo<v8::Value>& info) {
  v8::Handle<v8::Object> object;
  if (!fast_return_value_object_is_empty) {
    object = Object::New(info.GetIsolate());
  }
  info.GetReturnValue().Set(object);
}

template<typename T>
Handle<Value> TestFastReturnValues() {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::EscapableHandleScope scope(isolate);
  v8::Handle<v8::ObjectTemplate> object_template =
      v8::ObjectTemplate::New(isolate);
  v8::FunctionCallback callback = &FastReturnValueCallback<T>;
  object_template->Set(isolate, "callback",
                       v8::FunctionTemplate::New(isolate, callback));
  v8::Local<v8::Object> object = object_template->NewInstance();
  (*env)->Global()->Set(v8_str("callback_object"), object);
  return scope.Escape(CompileRun("callback_object.callback()"));
}


THREADED_PROFILED_TEST(FastReturnValues) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);
  v8::Handle<v8::Value> value;
  // check int32_t and uint32_t
  int32_t int_values[] = {
      0, 234, -723,
      i::Smi::kMinValue, i::Smi::kMaxValue
  };
  for (size_t i = 0; i < arraysize(int_values); i++) {
    for (int modifier = -1; modifier <= 1; modifier++) {
      int int_value = int_values[i] + modifier;
      // check int32_t
      fast_return_value_int32 = int_value;
      value = TestFastReturnValues<int32_t>();
      CHECK(value->IsInt32());
      CHECK(fast_return_value_int32 == value->Int32Value());
      // check uint32_t
      fast_return_value_uint32 = static_cast<uint32_t>(int_value);
      value = TestFastReturnValues<uint32_t>();
      CHECK(value->IsUint32());
      CHECK(fast_return_value_uint32 == value->Uint32Value());
    }
  }
  // check double
  value = TestFastReturnValues<double>();
  CHECK(value->IsNumber());
  CHECK_EQ(kFastReturnValueDouble, value->ToNumber(isolate)->Value());
  // check bool values
  for (int i = 0; i < 2; i++) {
    fast_return_value_bool = i == 0;
    value = TestFastReturnValues<bool>();
    CHECK(value->IsBoolean());
    CHECK_EQ(fast_return_value_bool, value->ToBoolean(isolate)->Value());
  }
  // check oddballs
  ReturnValueOddball oddballs[] = {
      kNullReturnValue,
      kUndefinedReturnValue,
      kEmptyStringReturnValue
  };
  for (size_t i = 0; i < arraysize(oddballs); i++) {
    fast_return_value_void = oddballs[i];
    value = TestFastReturnValues<void>();
    switch (fast_return_value_void) {
      case kNullReturnValue:
        CHECK(value->IsNull());
        break;
      case kUndefinedReturnValue:
        CHECK(value->IsUndefined());
        break;
      case kEmptyStringReturnValue:
        CHECK(value->IsString());
        CHECK_EQ(0, v8::String::Cast(*value)->Length());
        break;
    }
  }
  // check handles
  fast_return_value_object_is_empty = false;
  value = TestFastReturnValues<Object>();
  CHECK(value->IsObject());
  fast_return_value_object_is_empty = true;
  value = TestFastReturnValues<Object>();
  CHECK(value->IsUndefined());
}


THREADED_TEST(FunctionTemplateSetLength) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);
  {
    Local<v8::FunctionTemplate> fun_templ =
        v8::FunctionTemplate::New(isolate,
                                  handle_callback,
                                  Handle<v8::Value>(),
                                  Handle<v8::Signature>(),
                                  23);
    Local<Function> fun = fun_templ->GetFunction();
    env->Global()->Set(v8_str("obj"), fun);
    Local<Script> script = v8_compile("obj.length");
    CHECK_EQ(23, script->Run()->Int32Value());
  }
  {
    Local<v8::FunctionTemplate> fun_templ =
        v8::FunctionTemplate::New(isolate, handle_callback);
    fun_templ->SetLength(22);
    Local<Function> fun = fun_templ->GetFunction();
    env->Global()->Set(v8_str("obj"), fun);
    Local<Script> script = v8_compile("obj.length");
    CHECK_EQ(22, script->Run()->Int32Value());
  }
  {
    // Without setting length it defaults to 0.
    Local<v8::FunctionTemplate> fun_templ =
        v8::FunctionTemplate::New(isolate, handle_callback);
    Local<Function> fun = fun_templ->GetFunction();
    env->Global()->Set(v8_str("obj"), fun);
    Local<Script> script = v8_compile("obj.length");
    CHECK_EQ(0, script->Run()->Int32Value());
  }
}


static void* expected_ptr;
static void callback(const v8::FunctionCallbackInfo<v8::Value>& args) {
  void* ptr = v8::External::Cast(*args.Data())->Value();
  CHECK_EQ(expected_ptr, ptr);
  args.GetReturnValue().Set(true);
}


static void TestExternalPointerWrapping() {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);

  v8::Handle<v8::Value> data =
      v8::External::New(isolate, expected_ptr);

  v8::Handle<v8::Object> obj = v8::Object::New(isolate);
  obj->Set(v8_str("func"),
           v8::FunctionTemplate::New(isolate, callback, data)->GetFunction());
  env->Global()->Set(v8_str("obj"), obj);

  CHECK(CompileRun(
        "function foo() {\n"
        "  for (var i = 0; i < 13; i++) obj.func();\n"
        "}\n"
        "foo(), true")->BooleanValue());
}


THREADED_TEST(ExternalWrap) {
  // Check heap allocated object.
  int* ptr = new int;
  expected_ptr = ptr;
  TestExternalPointerWrapping();
  delete ptr;

  // Check stack allocated object.
  int foo;
  expected_ptr = &foo;
  TestExternalPointerWrapping();

  // Check not aligned addresses.
  const int n = 100;
  char* s = new char[n];
  for (int i = 0; i < n; i++) {
    expected_ptr = s + i;
    TestExternalPointerWrapping();
  }

  delete[] s;

  // Check several invalid addresses.
  expected_ptr = reinterpret_cast<void*>(1);
  TestExternalPointerWrapping();

  expected_ptr = reinterpret_cast<void*>(0xdeadbeef);
  TestExternalPointerWrapping();

  expected_ptr = reinterpret_cast<void*>(0xdeadbeef + 1);
  TestExternalPointerWrapping();

#if defined(V8_HOST_ARCH_X64)
  // Check a value with a leading 1 bit in x64 Smi encoding.
  expected_ptr = reinterpret_cast<void*>(0x400000000);
  TestExternalPointerWrapping();

  expected_ptr = reinterpret_cast<void*>(0xdeadbeefdeadbeef);
  TestExternalPointerWrapping();

  expected_ptr = reinterpret_cast<void*>(0xdeadbeefdeadbeef + 1);
  TestExternalPointerWrapping();
#endif
}


THREADED_TEST(FindInstanceInPrototypeChain) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);

  Local<v8::FunctionTemplate> base = v8::FunctionTemplate::New(isolate);
  Local<v8::FunctionTemplate> derived = v8::FunctionTemplate::New(isolate);
  Local<v8::FunctionTemplate> other = v8::FunctionTemplate::New(isolate);
  derived->Inherit(base);

  Local<v8::Function> base_function = base->GetFunction();
  Local<v8::Function> derived_function = derived->GetFunction();
  Local<v8::Function> other_function = other->GetFunction();

  Local<v8::Object> base_instance = base_function->NewInstance();
  Local<v8::Object> derived_instance = derived_function->NewInstance();
  Local<v8::Object> derived_instance2 = derived_function->NewInstance();
  Local<v8::Object> other_instance = other_function->NewInstance();
  derived_instance2->Set(v8_str("__proto__"), derived_instance);
  other_instance->Set(v8_str("__proto__"), derived_instance2);

  // base_instance is only an instance of base.
  CHECK(
      base_instance->Equals(base_instance->FindInstanceInPrototypeChain(base)));
  CHECK(base_instance->FindInstanceInPrototypeChain(derived).IsEmpty());
  CHECK(base_instance->FindInstanceInPrototypeChain(other).IsEmpty());

  // derived_instance is an instance of base and derived.
  CHECK(derived_instance->Equals(
      derived_instance->FindInstanceInPrototypeChain(base)));
  CHECK(derived_instance->Equals(
      derived_instance->FindInstanceInPrototypeChain(derived)));
  CHECK(derived_instance->FindInstanceInPrototypeChain(other).IsEmpty());

  // other_instance is an instance of other and its immediate
  // prototype derived_instance2 is an instance of base and derived.
  // Note, derived_instance is an instance of base and derived too,
  // but it comes after derived_instance2 in the prototype chain of
  // other_instance.
  CHECK(derived_instance2->Equals(
      other_instance->FindInstanceInPrototypeChain(base)));
  CHECK(derived_instance2->Equals(
      other_instance->FindInstanceInPrototypeChain(derived)));
  CHECK(other_instance->Equals(
      other_instance->FindInstanceInPrototypeChain(other)));
}


THREADED_TEST(TinyInteger) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);

  int32_t value = 239;
  Local<v8::Integer> value_obj = v8::Integer::New(isolate, value);
  CHECK_EQ(static_cast<int64_t>(value), value_obj->Value());

  value_obj = v8::Integer::New(isolate, value);
  CHECK_EQ(static_cast<int64_t>(value), value_obj->Value());
}


THREADED_TEST(BigSmiInteger) {
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());
  v8::Isolate* isolate = CcTest::isolate();

  int32_t value = i::Smi::kMaxValue;
  // We cannot add one to a Smi::kMaxValue without wrapping.
  if (i::SmiValuesAre31Bits()) {
    CHECK(i::Smi::IsValid(value));
    CHECK(!i::Smi::IsValid(value + 1));

    Local<v8::Integer> value_obj = v8::Integer::New(isolate, value);
    CHECK_EQ(static_cast<int64_t>(value), value_obj->Value());

    value_obj = v8::Integer::New(isolate, value);
    CHECK_EQ(static_cast<int64_t>(value), value_obj->Value());
  }
}


THREADED_TEST(BigInteger) {
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());
  v8::Isolate* isolate = CcTest::isolate();

  // We cannot add one to a Smi::kMaxValue without wrapping.
  if (i::SmiValuesAre31Bits()) {
    // The casts allow this to compile, even if Smi::kMaxValue is 2^31-1.
    // The code will not be run in that case, due to the "if" guard.
    int32_t value =
        static_cast<int32_t>(static_cast<uint32_t>(i::Smi::kMaxValue) + 1);
    CHECK(value > i::Smi::kMaxValue);
    CHECK(!i::Smi::IsValid(value));

    Local<v8::Integer> value_obj = v8::Integer::New(isolate, value);
    CHECK_EQ(static_cast<int64_t>(value), value_obj->Value());

    value_obj = v8::Integer::New(isolate, value);
    CHECK_EQ(static_cast<int64_t>(value), value_obj->Value());
  }
}


THREADED_TEST(TinyUnsignedInteger) {
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());
  v8::Isolate* isolate = CcTest::isolate();

  uint32_t value = 239;

  Local<v8::Integer> value_obj = v8::Integer::NewFromUnsigned(isolate, value);
  CHECK_EQ(static_cast<int64_t>(value), value_obj->Value());

  value_obj = v8::Integer::NewFromUnsigned(isolate, value);
  CHECK_EQ(static_cast<int64_t>(value), value_obj->Value());
}


THREADED_TEST(BigUnsignedSmiInteger) {
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());
  v8::Isolate* isolate = CcTest::isolate();

  uint32_t value = static_cast<uint32_t>(i::Smi::kMaxValue);
  CHECK(i::Smi::IsValid(value));
  CHECK(!i::Smi::IsValid(value + 1));

  Local<v8::Integer> value_obj = v8::Integer::NewFromUnsigned(isolate, value);
  CHECK_EQ(static_cast<int64_t>(value), value_obj->Value());

  value_obj = v8::Integer::NewFromUnsigned(isolate, value);
  CHECK_EQ(static_cast<int64_t>(value), value_obj->Value());
}


THREADED_TEST(BigUnsignedInteger) {
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());
  v8::Isolate* isolate = CcTest::isolate();

  uint32_t value = static_cast<uint32_t>(i::Smi::kMaxValue) + 1;
  CHECK(value > static_cast<uint32_t>(i::Smi::kMaxValue));
  CHECK(!i::Smi::IsValid(value));

  Local<v8::Integer> value_obj = v8::Integer::NewFromUnsigned(isolate, value);
  CHECK_EQ(static_cast<int64_t>(value), value_obj->Value());

  value_obj = v8::Integer::NewFromUnsigned(isolate, value);
  CHECK_EQ(static_cast<int64_t>(value), value_obj->Value());
}


THREADED_TEST(OutOfSignedRangeUnsignedInteger) {
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());
  v8::Isolate* isolate = CcTest::isolate();

  uint32_t INT32_MAX_AS_UINT = (1U << 31) - 1;
  uint32_t value = INT32_MAX_AS_UINT + 1;
  CHECK(value > INT32_MAX_AS_UINT);  // No overflow.

  Local<v8::Integer> value_obj = v8::Integer::NewFromUnsigned(isolate, value);
  CHECK_EQ(static_cast<int64_t>(value), value_obj->Value());

  value_obj = v8::Integer::NewFromUnsigned(isolate, value);
  CHECK_EQ(static_cast<int64_t>(value), value_obj->Value());
}


THREADED_TEST(IsNativeError) {
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());
  v8::Handle<Value> syntax_error = CompileRun(
      "var out = 0; try { eval(\"#\"); } catch(x) { out = x; } out; ");
  CHECK(syntax_error->IsNativeError());
  v8::Handle<Value> not_error = CompileRun("{a:42}");
  CHECK(!not_error->IsNativeError());
  v8::Handle<Value> not_object = CompileRun("42");
  CHECK(!not_object->IsNativeError());
}


THREADED_TEST(IsGeneratorFunctionOrObject) {
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());

  CompileRun("function *gen() { yield 1; }\nfunction func() {}");
  v8::Handle<Value> gen = CompileRun("gen");
  v8::Handle<Value> genObj = CompileRun("gen()");
  v8::Handle<Value> object = CompileRun("{a:42}");
  v8::Handle<Value> func = CompileRun("func");

  CHECK(gen->IsGeneratorFunction());
  CHECK(gen->IsFunction());
  CHECK(!gen->IsGeneratorObject());

  CHECK(!genObj->IsGeneratorFunction());
  CHECK(!genObj->IsFunction());
  CHECK(genObj->IsGeneratorObject());

  CHECK(!object->IsGeneratorFunction());
  CHECK(!object->IsFunction());
  CHECK(!object->IsGeneratorObject());

  CHECK(!func->IsGeneratorFunction());
  CHECK(func->IsFunction());
  CHECK(!func->IsGeneratorObject());
}


THREADED_TEST(ArgumentsObject) {
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());
  v8::Handle<Value> arguments_object =
      CompileRun("var out = 0; (function(){ out = arguments; })(1,2,3); out;");
  CHECK(arguments_object->IsArgumentsObject());
  v8::Handle<Value> array = CompileRun("[1,2,3]");
  CHECK(!array->IsArgumentsObject());
  v8::Handle<Value> object = CompileRun("{a:42}");
  CHECK(!object->IsArgumentsObject());
}


THREADED_TEST(IsMapOrSet) {
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());
  v8::Handle<Value> map = CompileRun("new Map()");
  v8::Handle<Value> set = CompileRun("new Set()");
  v8::Handle<Value> weak_map = CompileRun("new WeakMap()");
  v8::Handle<Value> weak_set = CompileRun("new WeakSet()");
  CHECK(map->IsMap());
  CHECK(set->IsSet());
  CHECK(weak_map->IsWeakMap());
  CHECK(weak_set->IsWeakSet());

  CHECK(!map->IsSet());
  CHECK(!map->IsWeakMap());
  CHECK(!map->IsWeakSet());

  CHECK(!set->IsMap());
  CHECK(!set->IsWeakMap());
  CHECK(!set->IsWeakSet());

  CHECK(!weak_map->IsMap());
  CHECK(!weak_map->IsSet());
  CHECK(!weak_map->IsWeakSet());

  CHECK(!weak_set->IsMap());
  CHECK(!weak_set->IsSet());
  CHECK(!weak_set->IsWeakMap());

  v8::Handle<Value> object = CompileRun("{a:42}");
  CHECK(!object->IsMap());
  CHECK(!object->IsSet());
  CHECK(!object->IsWeakMap());
  CHECK(!object->IsWeakSet());
}


THREADED_TEST(StringObject) {
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());
  v8::Handle<Value> boxed_string = CompileRun("new String(\"test\")");
  CHECK(boxed_string->IsStringObject());
  v8::Handle<Value> unboxed_string = CompileRun("\"test\"");
  CHECK(!unboxed_string->IsStringObject());
  v8::Handle<Value> boxed_not_string = CompileRun("new Number(42)");
  CHECK(!boxed_not_string->IsStringObject());
  v8::Handle<Value> not_object = CompileRun("0");
  CHECK(!not_object->IsStringObject());
  v8::Handle<v8::StringObject> as_boxed = boxed_string.As<v8::StringObject>();
  CHECK(!as_boxed.IsEmpty());
  Local<v8::String> the_string = as_boxed->ValueOf();
  CHECK(!the_string.IsEmpty());
  ExpectObject("\"test\"", the_string);
  v8::Handle<v8::Value> new_boxed_string = v8::StringObject::New(the_string);
  CHECK(new_boxed_string->IsStringObject());
  as_boxed = new_boxed_string.As<v8::StringObject>();
  the_string = as_boxed->ValueOf();
  CHECK(!the_string.IsEmpty());
  ExpectObject("\"test\"", the_string);
}


THREADED_TEST(NumberObject) {
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());
  v8::Handle<Value> boxed_number = CompileRun("new Number(42)");
  CHECK(boxed_number->IsNumberObject());
  v8::Handle<Value> unboxed_number = CompileRun("42");
  CHECK(!unboxed_number->IsNumberObject());
  v8::Handle<Value> boxed_not_number = CompileRun("new Boolean(false)");
  CHECK(!boxed_not_number->IsNumberObject());
  v8::Handle<v8::NumberObject> as_boxed = boxed_number.As<v8::NumberObject>();
  CHECK(!as_boxed.IsEmpty());
  double the_number = as_boxed->ValueOf();
  CHECK_EQ(42.0, the_number);
  v8::Handle<v8::Value> new_boxed_number =
      v8::NumberObject::New(env->GetIsolate(), 43);
  CHECK(new_boxed_number->IsNumberObject());
  as_boxed = new_boxed_number.As<v8::NumberObject>();
  the_number = as_boxed->ValueOf();
  CHECK_EQ(43.0, the_number);
}


THREADED_TEST(BooleanObject) {
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());
  v8::Handle<Value> boxed_boolean = CompileRun("new Boolean(true)");
  CHECK(boxed_boolean->IsBooleanObject());
  v8::Handle<Value> unboxed_boolean = CompileRun("true");
  CHECK(!unboxed_boolean->IsBooleanObject());
  v8::Handle<Value> boxed_not_boolean = CompileRun("new Number(42)");
  CHECK(!boxed_not_boolean->IsBooleanObject());
  v8::Handle<v8::BooleanObject> as_boxed =
      boxed_boolean.As<v8::BooleanObject>();
  CHECK(!as_boxed.IsEmpty());
  bool the_boolean = as_boxed->ValueOf();
  CHECK_EQ(true, the_boolean);
  v8::Handle<v8::Value> boxed_true = v8::BooleanObject::New(true);
  v8::Handle<v8::Value> boxed_false = v8::BooleanObject::New(false);
  CHECK(boxed_true->IsBooleanObject());
  CHECK(boxed_false->IsBooleanObject());
  as_boxed = boxed_true.As<v8::BooleanObject>();
  CHECK_EQ(true, as_boxed->ValueOf());
  as_boxed = boxed_false.As<v8::BooleanObject>();
  CHECK_EQ(false, as_boxed->ValueOf());
}


THREADED_TEST(PrimitiveAndWrappedBooleans) {
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());

  Local<Value> primitive_false = Boolean::New(env->GetIsolate(), false);
  CHECK(primitive_false->IsBoolean());
  CHECK(!primitive_false->IsBooleanObject());
  CHECK(!primitive_false->BooleanValue());
  CHECK(!primitive_false->IsTrue());
  CHECK(primitive_false->IsFalse());

  Local<Value> false_value = BooleanObject::New(false);
  CHECK(!false_value->IsBoolean());
  CHECK(false_value->IsBooleanObject());
  CHECK(false_value->BooleanValue());
  CHECK(!false_value->IsTrue());
  CHECK(!false_value->IsFalse());

  Local<BooleanObject> false_boolean_object = false_value.As<BooleanObject>();
  CHECK(!false_boolean_object->IsBoolean());
  CHECK(false_boolean_object->IsBooleanObject());
  // TODO(svenpanne) Uncomment when BooleanObject::BooleanValue() is deleted.
  // CHECK(false_boolean_object->BooleanValue());
  CHECK(!false_boolean_object->ValueOf());
  CHECK(!false_boolean_object->IsTrue());
  CHECK(!false_boolean_object->IsFalse());

  Local<Value> primitive_true = Boolean::New(env->GetIsolate(), true);
  CHECK(primitive_true->IsBoolean());
  CHECK(!primitive_true->IsBooleanObject());
  CHECK(primitive_true->BooleanValue());
  CHECK(primitive_true->IsTrue());
  CHECK(!primitive_true->IsFalse());

  Local<Value> true_value = BooleanObject::New(true);
  CHECK(!true_value->IsBoolean());
  CHECK(true_value->IsBooleanObject());
  CHECK(true_value->BooleanValue());
  CHECK(!true_value->IsTrue());
  CHECK(!true_value->IsFalse());

  Local<BooleanObject> true_boolean_object = true_value.As<BooleanObject>();
  CHECK(!true_boolean_object->IsBoolean());
  CHECK(true_boolean_object->IsBooleanObject());
  // TODO(svenpanne) Uncomment when BooleanObject::BooleanValue() is deleted.
  // CHECK(true_boolean_object->BooleanValue());
  CHECK(true_boolean_object->ValueOf());
  CHECK(!true_boolean_object->IsTrue());
  CHECK(!true_boolean_object->IsFalse());
}


THREADED_TEST(Number) {
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());
  double PI = 3.1415926;
  Local<v8::Number> pi_obj = v8::Number::New(env->GetIsolate(), PI);
  CHECK_EQ(PI, pi_obj->NumberValue());
}


THREADED_TEST(ToNumber) {
  LocalContext env;
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  Local<String> str = v8_str("3.1415926");
  CHECK_EQ(3.1415926, str->NumberValue());
  v8::Handle<v8::Boolean> t = v8::True(isolate);
  CHECK_EQ(1.0, t->NumberValue());
  v8::Handle<v8::Boolean> f = v8::False(isolate);
  CHECK_EQ(0.0, f->NumberValue());
}


THREADED_TEST(Date) {
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());
  double PI = 3.1415926;
  Local<Value> date = v8::Date::New(env->GetIsolate(), PI);
  CHECK_EQ(3.0, date->NumberValue());
  date.As<v8::Date>()->Set(v8_str("property"),
                           v8::Integer::New(env->GetIsolate(), 42));
  CHECK_EQ(42, date.As<v8::Date>()->Get(v8_str("property"))->Int32Value());
}


THREADED_TEST(Boolean) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);
  v8::Handle<v8::Boolean> t = v8::True(isolate);
  CHECK(t->Value());
  v8::Handle<v8::Boolean> f = v8::False(isolate);
  CHECK(!f->Value());
  v8::Handle<v8::Primitive> u = v8::Undefined(isolate);
  CHECK(!u->BooleanValue());
  v8::Handle<v8::Primitive> n = v8::Null(isolate);
  CHECK(!n->BooleanValue());
  v8::Handle<String> str1 = v8_str("");
  CHECK(!str1->BooleanValue());
  v8::Handle<String> str2 = v8_str("x");
  CHECK(str2->BooleanValue());
  CHECK(!v8::Number::New(isolate, 0)->BooleanValue());
  CHECK(v8::Number::New(isolate, -1)->BooleanValue());
  CHECK(v8::Number::New(isolate, 1)->BooleanValue());
  CHECK(v8::Number::New(isolate, 42)->BooleanValue());
  CHECK(!v8_compile("NaN")->Run()->BooleanValue());
}


static void DummyCallHandler(const v8::FunctionCallbackInfo<v8::Value>& args) {
  ApiTestFuzzer::Fuzz();
  args.GetReturnValue().Set(v8_num(13.4));
}


static void GetM(Local<String> name,
                 const v8::PropertyCallbackInfo<v8::Value>& info) {
  ApiTestFuzzer::Fuzz();
  info.GetReturnValue().Set(v8_num(876));
}


THREADED_TEST(GlobalPrototype) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  v8::Handle<v8::FunctionTemplate> func_templ =
      v8::FunctionTemplate::New(isolate);
  func_templ->PrototypeTemplate()->Set(
      isolate, "dummy", v8::FunctionTemplate::New(isolate, DummyCallHandler));
  v8::Handle<ObjectTemplate> templ = func_templ->InstanceTemplate();
  templ->Set(isolate, "x", v8_num(200));
  templ->SetAccessor(v8_str("m"), GetM);
  LocalContext env(0, templ);
  v8::Handle<Script> script(v8_compile("dummy()"));
  v8::Handle<Value> result(script->Run());
  CHECK_EQ(13.4, result->NumberValue());
  CHECK_EQ(200, v8_compile("x")->Run()->Int32Value());
  CHECK_EQ(876, v8_compile("m")->Run()->Int32Value());
}


THREADED_TEST(ObjectTemplate) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  Local<ObjectTemplate> templ1 = ObjectTemplate::New(isolate);
  templ1->Set(isolate, "x", v8_num(10));
  templ1->Set(isolate, "y", v8_num(13));
  LocalContext env;
  Local<v8::Object> instance1 = templ1->NewInstance();
  env->Global()->Set(v8_str("p"), instance1);
  CHECK(v8_compile("(p.x == 10)")->Run()->BooleanValue());
  CHECK(v8_compile("(p.y == 13)")->Run()->BooleanValue());
  Local<v8::FunctionTemplate> fun = v8::FunctionTemplate::New(isolate);
  fun->PrototypeTemplate()->Set(isolate, "nirk", v8_num(123));
  Local<ObjectTemplate> templ2 = fun->InstanceTemplate();
  templ2->Set(isolate, "a", v8_num(12));
  templ2->Set(isolate, "b", templ1);
  Local<v8::Object> instance2 = templ2->NewInstance();
  env->Global()->Set(v8_str("q"), instance2);
  CHECK(v8_compile("(q.nirk == 123)")->Run()->BooleanValue());
  CHECK(v8_compile("(q.a == 12)")->Run()->BooleanValue());
  CHECK(v8_compile("(q.b.x == 10)")->Run()->BooleanValue());
  CHECK(v8_compile("(q.b.y == 13)")->Run()->BooleanValue());
}


static void GetFlabby(const v8::FunctionCallbackInfo<v8::Value>& args) {
  ApiTestFuzzer::Fuzz();
  args.GetReturnValue().Set(v8_num(17.2));
}


static void GetKnurd(Local<String> property,
                     const v8::PropertyCallbackInfo<v8::Value>& info) {
  ApiTestFuzzer::Fuzz();
  info.GetReturnValue().Set(v8_num(15.2));
}


THREADED_TEST(DescriptorInheritance) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  v8::Handle<v8::FunctionTemplate> super = v8::FunctionTemplate::New(isolate);
  super->PrototypeTemplate()->Set(isolate, "flabby",
                                  v8::FunctionTemplate::New(isolate,
                                                            GetFlabby));
  super->PrototypeTemplate()->Set(isolate, "PI", v8_num(3.14));

  super->InstanceTemplate()->SetAccessor(v8_str("knurd"), GetKnurd);

  v8::Handle<v8::FunctionTemplate> base1 = v8::FunctionTemplate::New(isolate);
  base1->Inherit(super);
  base1->PrototypeTemplate()->Set(isolate, "v1", v8_num(20.1));

  v8::Handle<v8::FunctionTemplate> base2 = v8::FunctionTemplate::New(isolate);
  base2->Inherit(super);
  base2->PrototypeTemplate()->Set(isolate, "v2", v8_num(10.1));

  LocalContext env;

  env->Global()->Set(v8_str("s"), super->GetFunction());
  env->Global()->Set(v8_str("base1"), base1->GetFunction());
  env->Global()->Set(v8_str("base2"), base2->GetFunction());

  // Checks right __proto__ chain.
  CHECK(CompileRun("base1.prototype.__proto__ == s.prototype")->BooleanValue());
  CHECK(CompileRun("base2.prototype.__proto__ == s.prototype")->BooleanValue());

  CHECK(v8_compile("s.prototype.PI == 3.14")->Run()->BooleanValue());

  // Instance accessor should not be visible on function object or its prototype
  CHECK(CompileRun("s.knurd == undefined")->BooleanValue());
  CHECK(CompileRun("s.prototype.knurd == undefined")->BooleanValue());
  CHECK(CompileRun("base1.prototype.knurd == undefined")->BooleanValue());

  env->Global()->Set(v8_str("obj"),
                     base1->GetFunction()->NewInstance());
  CHECK_EQ(17.2, v8_compile("obj.flabby()")->Run()->NumberValue());
  CHECK(v8_compile("'flabby' in obj")->Run()->BooleanValue());
  CHECK_EQ(15.2, v8_compile("obj.knurd")->Run()->NumberValue());
  CHECK(v8_compile("'knurd' in obj")->Run()->BooleanValue());
  CHECK_EQ(20.1, v8_compile("obj.v1")->Run()->NumberValue());

  env->Global()->Set(v8_str("obj2"),
                     base2->GetFunction()->NewInstance());
  CHECK_EQ(17.2, v8_compile("obj2.flabby()")->Run()->NumberValue());
  CHECK(v8_compile("'flabby' in obj2")->Run()->BooleanValue());
  CHECK_EQ(15.2, v8_compile("obj2.knurd")->Run()->NumberValue());
  CHECK(v8_compile("'knurd' in obj2")->Run()->BooleanValue());
  CHECK_EQ(10.1, v8_compile("obj2.v2")->Run()->NumberValue());

  // base1 and base2 cannot cross reference to each's prototype
  CHECK(v8_compile("obj.v2")->Run()->IsUndefined());
  CHECK(v8_compile("obj2.v1")->Run()->IsUndefined());
}


// Helper functions for Interceptor/Accessor interaction tests

void SimpleAccessorGetter(Local<String> name,
                          const v8::PropertyCallbackInfo<v8::Value>& info) {
  Handle<Object> self = Handle<Object>::Cast(info.This());
  info.GetReturnValue().Set(
      self->Get(String::Concat(v8_str("accessor_"), name)));
}

void SimpleAccessorSetter(Local<String> name, Local<Value> value,
                          const v8::PropertyCallbackInfo<void>& info) {
  Handle<Object> self = Handle<Object>::Cast(info.This());
  self->Set(String::Concat(v8_str("accessor_"), name), value);
}

void SymbolAccessorGetter(Local<Name> name,
                          const v8::PropertyCallbackInfo<v8::Value>& info) {
  CHECK(name->IsSymbol());
  Local<Symbol> sym = Local<Symbol>::Cast(name);
  if (sym->Name()->IsUndefined())
    return;
  SimpleAccessorGetter(Local<String>::Cast(sym->Name()), info);
}

void SymbolAccessorSetter(Local<Name> name, Local<Value> value,
                          const v8::PropertyCallbackInfo<void>& info) {
  CHECK(name->IsSymbol());
  Local<Symbol> sym = Local<Symbol>::Cast(name);
  if (sym->Name()->IsUndefined())
    return;
  SimpleAccessorSetter(Local<String>::Cast(sym->Name()), value, info);
}

void SymbolAccessorGetterReturnsDefault(
    Local<Name> name, const v8::PropertyCallbackInfo<v8::Value>& info) {
  CHECK(name->IsSymbol());
  Local<Symbol> sym = Local<Symbol>::Cast(name);
  if (sym->Name()->IsUndefined()) return;
  info.GetReturnValue().Set(info.Data());
}

static void ThrowingSymbolAccessorGetter(
    Local<Name> name, const v8::PropertyCallbackInfo<v8::Value>& info) {
  info.GetReturnValue().Set(info.GetIsolate()->ThrowException(name));
}


THREADED_TEST(ExecutableAccessorIsPreservedOnAttributeChange) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  LocalContext env;
  v8::Local<v8::Value> res = CompileRun("var a = []; a;");
  i::Handle<i::JSObject> a(v8::Utils::OpenHandle(v8::Object::Cast(*res)));
  CHECK(a->map()->instance_descriptors()->IsFixedArray());
  CHECK_GT(i::FixedArray::cast(a->map()->instance_descriptors())->length(), 0);
  CompileRun("Object.defineProperty(a, 'length', { writable: false });");
  CHECK_EQ(i::FixedArray::cast(a->map()->instance_descriptors())->length(), 0);
  // But we should still have an ExecutableAccessorInfo.
  i::Handle<i::String> name(v8::Utils::OpenHandle(*v8_str("length")));
  i::LookupIterator it(a, name, i::LookupIterator::OWN_SKIP_INTERCEPTOR);
  CHECK_EQ(i::LookupIterator::ACCESSOR, it.state());
  CHECK(it.GetAccessors()->IsExecutableAccessorInfo());
}


THREADED_TEST(UndefinedIsNotEnumerable) {
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());
  v8::Handle<Value> result = CompileRun("this.propertyIsEnumerable(undefined)");
  CHECK(result->IsFalse());
}


v8::Handle<Script> call_recursively_script;
static const int kTargetRecursionDepth = 200;  // near maximum


static void CallScriptRecursivelyCall(
    const v8::FunctionCallbackInfo<v8::Value>& args) {
  ApiTestFuzzer::Fuzz();
  int depth = args.This()->Get(v8_str("depth"))->Int32Value();
  if (depth == kTargetRecursionDepth) return;
  args.This()->Set(v8_str("depth"),
                   v8::Integer::New(args.GetIsolate(), depth + 1));
  args.GetReturnValue().Set(call_recursively_script->Run());
}


static void CallFunctionRecursivelyCall(
    const v8::FunctionCallbackInfo<v8::Value>& args) {
  ApiTestFuzzer::Fuzz();
  int depth = args.This()->Get(v8_str("depth"))->Int32Value();
  if (depth == kTargetRecursionDepth) {
    printf("[depth = %d]\n", depth);
    return;
  }
  args.This()->Set(v8_str("depth"),
                   v8::Integer::New(args.GetIsolate(), depth + 1));
  v8::Handle<Value> function =
      args.This()->Get(v8_str("callFunctionRecursively"));
  args.GetReturnValue().Set(
      function.As<Function>()->Call(args.This(), 0, NULL));
}


THREADED_TEST(DeepCrossLanguageRecursion) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  v8::Handle<v8::ObjectTemplate> global = ObjectTemplate::New(isolate);
  global->Set(v8_str("callScriptRecursively"),
              v8::FunctionTemplate::New(isolate, CallScriptRecursivelyCall));
  global->Set(v8_str("callFunctionRecursively"),
              v8::FunctionTemplate::New(isolate, CallFunctionRecursivelyCall));
  LocalContext env(NULL, global);

  env->Global()->Set(v8_str("depth"), v8::Integer::New(isolate, 0));
  call_recursively_script = v8_compile("callScriptRecursively()");
  call_recursively_script->Run();
  call_recursively_script = v8::Handle<Script>();

  env->Global()->Set(v8_str("depth"), v8::Integer::New(isolate, 0));
  CompileRun("callFunctionRecursively()");
}


static void ThrowingPropertyHandlerGet(
    Local<Name> key, const v8::PropertyCallbackInfo<v8::Value>& info) {
  // Since this interceptor is used on "with" objects, the runtime will look up
  // @@unscopables.  Punt.
  if (key->IsSymbol()) return;
  ApiTestFuzzer::Fuzz();
  info.GetReturnValue().Set(info.GetIsolate()->ThrowException(key));
}


static void ThrowingPropertyHandlerSet(
    Local<Name> key, Local<Value>,
    const v8::PropertyCallbackInfo<v8::Value>& info) {
  info.GetIsolate()->ThrowException(key);
  info.GetReturnValue().SetUndefined();  // not the same as empty handle
}


THREADED_TEST(CallbackExceptionRegression) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  v8::Handle<v8::ObjectTemplate> obj = ObjectTemplate::New(isolate);
  obj->SetHandler(v8::NamedPropertyHandlerConfiguration(
      ThrowingPropertyHandlerGet, ThrowingPropertyHandlerSet));
  LocalContext env;
  env->Global()->Set(v8_str("obj"), obj->NewInstance());
  v8::Handle<Value> otto =
      CompileRun("try { with (obj) { otto; } } catch (e) { e; }");
  CHECK(v8_str("otto")->Equals(otto));
  v8::Handle<Value> netto =
      CompileRun("try { with (obj) { netto = 4; } } catch (e) { e; }");
  CHECK(v8_str("netto")->Equals(netto));
}


THREADED_TEST(FunctionPrototype) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  Local<v8::FunctionTemplate> Foo = v8::FunctionTemplate::New(isolate);
  Foo->PrototypeTemplate()->Set(v8_str("plak"), v8_num(321));
  LocalContext env;
  env->Global()->Set(v8_str("Foo"), Foo->GetFunction());
  Local<Script> script = v8_compile("Foo.prototype.plak");
  CHECK_EQ(script->Run()->Int32Value(), 321);
}


THREADED_TEST(InternalFields) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);

  Local<v8::FunctionTemplate> templ = v8::FunctionTemplate::New(isolate);
  Local<v8::ObjectTemplate> instance_templ = templ->InstanceTemplate();
  instance_templ->SetInternalFieldCount(1);
  Local<v8::Object> obj = templ->GetFunction()->NewInstance();
  CHECK_EQ(1, obj->InternalFieldCount());
  CHECK(obj->GetInternalField(0)->IsUndefined());
  obj->SetInternalField(0, v8_num(17));
  CHECK_EQ(17, obj->GetInternalField(0)->Int32Value());
}


THREADED_TEST(GlobalObjectInternalFields) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  Local<v8::ObjectTemplate> global_template = v8::ObjectTemplate::New(isolate);
  global_template->SetInternalFieldCount(1);
  LocalContext env(NULL, global_template);
  v8::Handle<v8::Object> global_proxy = env->Global();
  v8::Handle<v8::Object> global = global_proxy->GetPrototype().As<v8::Object>();
  CHECK_EQ(1, global->InternalFieldCount());
  CHECK(global->GetInternalField(0)->IsUndefined());
  global->SetInternalField(0, v8_num(17));
  CHECK_EQ(17, global->GetInternalField(0)->Int32Value());
}


THREADED_TEST(GlobalObjectHasRealIndexedProperty) {
  LocalContext env;
  v8::HandleScope scope(CcTest::isolate());

  v8::Local<v8::Object> global = env->Global();
  global->Set(0, v8::String::NewFromUtf8(CcTest::isolate(), "value"));
  CHECK(global->HasRealIndexedProperty(0));
}


static void CheckAlignedPointerInInternalField(Handle<v8::Object> obj,
                                               void* value) {
  CHECK_EQ(0, static_cast<int>(reinterpret_cast<uintptr_t>(value) & 0x1));
  obj->SetAlignedPointerInInternalField(0, value);
  CcTest::heap()->CollectAllGarbage(i::Heap::kNoGCFlags);
  CHECK_EQ(value, obj->GetAlignedPointerFromInternalField(0));
}


THREADED_TEST(InternalFieldsAlignedPointers) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);

  Local<v8::FunctionTemplate> templ = v8::FunctionTemplate::New(isolate);
  Local<v8::ObjectTemplate> instance_templ = templ->InstanceTemplate();
  instance_templ->SetInternalFieldCount(1);
  Local<v8::Object> obj = templ->GetFunction()->NewInstance();
  CHECK_EQ(1, obj->InternalFieldCount());

  CheckAlignedPointerInInternalField(obj, NULL);

  int* heap_allocated = new int[100];
  CheckAlignedPointerInInternalField(obj, heap_allocated);
  delete[] heap_allocated;

  int stack_allocated[100];
  CheckAlignedPointerInInternalField(obj, stack_allocated);

  void* huge = reinterpret_cast<void*>(~static_cast<uintptr_t>(1));
  CheckAlignedPointerInInternalField(obj, huge);

  v8::Global<v8::Object> persistent(isolate, obj);
  CHECK_EQ(1, Object::InternalFieldCount(persistent));
  CHECK_EQ(huge, Object::GetAlignedPointerFromInternalField(persistent, 0));
}


static void CheckAlignedPointerInEmbedderData(LocalContext* env, int index,
                                              void* value) {
  CHECK_EQ(0, static_cast<int>(reinterpret_cast<uintptr_t>(value) & 0x1));
  (*env)->SetAlignedPointerInEmbedderData(index, value);
  CcTest::heap()->CollectAllGarbage(i::Heap::kNoGCFlags);
  CHECK_EQ(value, (*env)->GetAlignedPointerFromEmbedderData(index));
}


static void* AlignedTestPointer(int i) {
  return reinterpret_cast<void*>(i * 1234);
}


THREADED_TEST(EmbedderDataAlignedPointers) {
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());

  CheckAlignedPointerInEmbedderData(&env, 0, NULL);

  int* heap_allocated = new int[100];
  CheckAlignedPointerInEmbedderData(&env, 1, heap_allocated);
  delete[] heap_allocated;

  int stack_allocated[100];
  CheckAlignedPointerInEmbedderData(&env, 2, stack_allocated);

  void* huge = reinterpret_cast<void*>(~static_cast<uintptr_t>(1));
  CheckAlignedPointerInEmbedderData(&env, 3, huge);

  // Test growing of the embedder data's backing store.
  for (int i = 0; i < 100; i++) {
    env->SetAlignedPointerInEmbedderData(i, AlignedTestPointer(i));
  }
  CcTest::heap()->CollectAllGarbage(i::Heap::kNoGCFlags);
  for (int i = 0; i < 100; i++) {
    CHECK_EQ(AlignedTestPointer(i), env->GetAlignedPointerFromEmbedderData(i));
  }
}


static void CheckEmbedderData(LocalContext* env, int index,
                              v8::Handle<Value> data) {
  (*env)->SetEmbedderData(index, data);
  CHECK((*env)->GetEmbedderData(index)->StrictEquals(data));
}


THREADED_TEST(EmbedderData) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);

  CheckEmbedderData(
      &env, 3, v8::String::NewFromUtf8(isolate, "The quick brown fox jumps"));
  CheckEmbedderData(&env, 2,
                    v8::String::NewFromUtf8(isolate, "over the lazy dog."));
  CheckEmbedderData(&env, 1, v8::Number::New(isolate, 1.2345));
  CheckEmbedderData(&env, 0, v8::Boolean::New(isolate, true));
}


THREADED_TEST(GetIsolate) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);
  Local<v8::Object> obj = v8::Object::New(isolate);
  CHECK_EQ(isolate, obj->GetIsolate());
  CHECK_EQ(isolate, CcTest::global()->GetIsolate());
}


THREADED_TEST(IdentityHash) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);

  // Ensure that the test starts with an fresh heap to test whether the hash
  // code is based on the address.
  CcTest::heap()->CollectAllGarbage(i::Heap::kNoGCFlags);
  Local<v8::Object> obj = v8::Object::New(isolate);
  int hash = obj->GetIdentityHash();
  int hash1 = obj->GetIdentityHash();
  CHECK_EQ(hash, hash1);
  int hash2 = v8::Object::New(isolate)->GetIdentityHash();
  // Since the identity hash is essentially a random number two consecutive
  // objects should not be assigned the same hash code. If the test below fails
  // the random number generator should be evaluated.
  CHECK_NE(hash, hash2);
  CcTest::heap()->CollectAllGarbage(i::Heap::kNoGCFlags);
  int hash3 = v8::Object::New(isolate)->GetIdentityHash();
  // Make sure that the identity hash is not based on the initial address of
  // the object alone. If the test below fails the random number generator
  // should be evaluated.
  CHECK_NE(hash, hash3);
  int hash4 = obj->GetIdentityHash();
  CHECK_EQ(hash, hash4);

  // Check identity hashes behaviour in the presence of JS accessors.
  // Put a getter for 'v8::IdentityHash' on the Object's prototype:
  {
    CompileRun("Object.prototype['v8::IdentityHash'] = 42;\n");
    Local<v8::Object> o1 = v8::Object::New(isolate);
    Local<v8::Object> o2 = v8::Object::New(isolate);
    CHECK_NE(o1->GetIdentityHash(), o2->GetIdentityHash());
  }
  {
    CompileRun(
        "function cnst() { return 42; };\n"
        "Object.prototype.__defineGetter__('v8::IdentityHash', cnst);\n");
    Local<v8::Object> o1 = v8::Object::New(isolate);
    Local<v8::Object> o2 = v8::Object::New(isolate);
    CHECK_NE(o1->GetIdentityHash(), o2->GetIdentityHash());
  }
}


THREADED_TEST(GlobalProxyIdentityHash) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);
  Handle<Object> global_proxy = env->Global();
  int hash1 = global_proxy->GetIdentityHash();
  // Hash should be retained after being detached.
  env->DetachGlobal();
  int hash2 = global_proxy->GetIdentityHash();
  CHECK_EQ(hash1, hash2);
  {
    // Re-attach global proxy to a new context, hash should stay the same.
    LocalContext env2(NULL, Handle<ObjectTemplate>(), global_proxy);
    int hash3 = global_proxy->GetIdentityHash();
    CHECK_EQ(hash1, hash3);
  }
}


TEST(SymbolIdentityHash) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);

  {
    Local<v8::Symbol> symbol = v8::Symbol::New(isolate);
    int hash = symbol->GetIdentityHash();
    int hash1 = symbol->GetIdentityHash();
    CHECK_EQ(hash, hash1);
    CcTest::heap()->CollectAllGarbage(i::Heap::kNoGCFlags);
    int hash3 = symbol->GetIdentityHash();
    CHECK_EQ(hash, hash3);
  }

  {
    v8::Handle<v8::Symbol> js_symbol =
        CompileRun("Symbol('foo')").As<v8::Symbol>();
    int hash = js_symbol->GetIdentityHash();
    int hash1 = js_symbol->GetIdentityHash();
    CHECK_EQ(hash, hash1);
    CcTest::heap()->CollectAllGarbage(i::Heap::kNoGCFlags);
    int hash3 = js_symbol->GetIdentityHash();
    CHECK_EQ(hash, hash3);
  }
}


TEST(StringIdentityHash) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);

  Local<v8::String> str = v8::String::NewFromUtf8(isolate, "str1");
  int hash = str->GetIdentityHash();
  int hash1 = str->GetIdentityHash();
  CHECK_EQ(hash, hash1);
  CcTest::heap()->CollectAllGarbage(i::Heap::kNoGCFlags);
  int hash3 = str->GetIdentityHash();
  CHECK_EQ(hash, hash3);

  Local<v8::String> str2 = v8::String::NewFromUtf8(isolate, "str1");
  int hash4 = str2->GetIdentityHash();
  CHECK_EQ(hash, hash4);
}


THREADED_TEST(SymbolProperties) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);

  v8::Local<v8::Object> obj = v8::Object::New(isolate);
  v8::Local<v8::Symbol> sym1 = v8::Symbol::New(isolate);
  v8::Local<v8::Symbol> sym2 = v8::Symbol::New(isolate, v8_str("my-symbol"));
  v8::Local<v8::Symbol> sym3 = v8::Symbol::New(isolate, v8_str("sym3"));

  CcTest::heap()->CollectAllGarbage(i::Heap::kNoGCFlags);

  // Check basic symbol functionality.
  CHECK(sym1->IsSymbol());
  CHECK(sym2->IsSymbol());
  CHECK(!obj->IsSymbol());

  CHECK(sym1->Equals(sym1));
  CHECK(sym2->Equals(sym2));
  CHECK(!sym1->Equals(sym2));
  CHECK(!sym2->Equals(sym1));
  CHECK(sym1->StrictEquals(sym1));
  CHECK(sym2->StrictEquals(sym2));
  CHECK(!sym1->StrictEquals(sym2));
  CHECK(!sym2->StrictEquals(sym1));

  CHECK(sym2->Name()->Equals(v8_str("my-symbol")));

  v8::Local<v8::Value> sym_val = sym2;
  CHECK(sym_val->IsSymbol());
  CHECK(sym_val->Equals(sym2));
  CHECK(sym_val->StrictEquals(sym2));
  CHECK(v8::Symbol::Cast(*sym_val)->Equals(sym2));

  v8::Local<v8::Value> sym_obj = v8::SymbolObject::New(isolate, sym2);
  CHECK(sym_obj->IsSymbolObject());
  CHECK(!sym2->IsSymbolObject());
  CHECK(!obj->IsSymbolObject());
  CHECK(!sym_obj->Equals(sym2));
  CHECK(!sym_obj->StrictEquals(sym2));
  CHECK(v8::SymbolObject::Cast(*sym_obj)->Equals(sym_obj));
  CHECK(v8::SymbolObject::Cast(*sym_obj)->ValueOf()->Equals(sym2));

  // Make sure delete of a non-existent symbol property works.
  CHECK(obj->Delete(sym1));
  CHECK(!obj->Has(sym1));

  CHECK(obj->Set(sym1, v8::Integer::New(isolate, 1503)));
  CHECK(obj->Has(sym1));
  CHECK_EQ(1503, obj->Get(sym1)->Int32Value());
  CHECK(obj->Set(sym1, v8::Integer::New(isolate, 2002)));
  CHECK(obj->Has(sym1));
  CHECK_EQ(2002, obj->Get(sym1)->Int32Value());
  CHECK_EQ(v8::None, obj->GetPropertyAttributes(sym1));

  CHECK_EQ(0u, obj->GetOwnPropertyNames()->Length());
  unsigned num_props = obj->GetPropertyNames()->Length();
  CHECK(obj->Set(v8::String::NewFromUtf8(isolate, "bla"),
                 v8::Integer::New(isolate, 20)));
  CHECK_EQ(1u, obj->GetOwnPropertyNames()->Length());
  CHECK_EQ(num_props + 1, obj->GetPropertyNames()->Length());

  CcTest::heap()->CollectAllGarbage(i::Heap::kNoGCFlags);

  CHECK(obj->SetAccessor(sym3, SymbolAccessorGetter, SymbolAccessorSetter));
  CHECK(obj->Get(sym3)->IsUndefined());
  CHECK(obj->Set(sym3, v8::Integer::New(isolate, 42)));
  CHECK(obj->Get(sym3)->Equals(v8::Integer::New(isolate, 42)));
  CHECK(obj->Get(v8::String::NewFromUtf8(isolate, "accessor_sym3"))
            ->Equals(v8::Integer::New(isolate, 42)));

  // Add another property and delete it afterwards to force the object in
  // slow case.
  CHECK(obj->Set(sym2, v8::Integer::New(isolate, 2008)));
  CHECK_EQ(2002, obj->Get(sym1)->Int32Value());
  CHECK_EQ(2008, obj->Get(sym2)->Int32Value());
  CHECK_EQ(2002, obj->Get(sym1)->Int32Value());
  CHECK_EQ(2u, obj->GetOwnPropertyNames()->Length());

  CHECK(obj->Has(sym1));
  CHECK(obj->Has(sym2));
  CHECK(obj->Has(sym3));
  CHECK(obj->Has(v8::String::NewFromUtf8(isolate, "accessor_sym3")));
  CHECK(obj->Delete(sym2));
  CHECK(obj->Has(sym1));
  CHECK(!obj->Has(sym2));
  CHECK(obj->Has(sym3));
  CHECK(obj->Has(v8::String::NewFromUtf8(isolate, "accessor_sym3")));
  CHECK_EQ(2002, obj->Get(sym1)->Int32Value());
  CHECK(obj->Get(sym3)->Equals(v8::Integer::New(isolate, 42)));
  CHECK(obj->Get(v8::String::NewFromUtf8(isolate, "accessor_sym3"))
            ->Equals(v8::Integer::New(isolate, 42)));
  CHECK_EQ(2u, obj->GetOwnPropertyNames()->Length());

  // Symbol properties are inherited.
  v8::Local<v8::Object> child = v8::Object::New(isolate);
  child->SetPrototype(obj);
  CHECK(child->Has(sym1));
  CHECK_EQ(2002, child->Get(sym1)->Int32Value());
  CHECK(obj->Get(sym3)->Equals(v8::Integer::New(isolate, 42)));
  CHECK(obj->Get(v8::String::NewFromUtf8(isolate, "accessor_sym3"))
            ->Equals(v8::Integer::New(isolate, 42)));
  CHECK_EQ(0u, child->GetOwnPropertyNames()->Length());
}


THREADED_TEST(SymbolTemplateProperties) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);
  v8::Local<v8::FunctionTemplate> foo = v8::FunctionTemplate::New(isolate);
  v8::Local<v8::Name> name = v8::Symbol::New(isolate);
  CHECK(!name.IsEmpty());
  foo->PrototypeTemplate()->Set(name, v8::FunctionTemplate::New(isolate));
  v8::Local<v8::Object> new_instance = foo->InstanceTemplate()->NewInstance();
  CHECK(!new_instance.IsEmpty());
  CHECK(new_instance->Has(name));
}


THREADED_TEST(PrivateProperties) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);

  v8::Local<v8::Object> obj = v8::Object::New(isolate);
  v8::Local<v8::Private> priv1 = v8::Private::New(isolate);
  v8::Local<v8::Private> priv2 =
      v8::Private::New(isolate, v8_str("my-private"));

  CcTest::heap()->CollectAllGarbage(i::Heap::kNoGCFlags);

  CHECK(priv2->Name()->Equals(v8::String::NewFromUtf8(isolate, "my-private")));

  // Make sure delete of a non-existent private symbol property works.
  CHECK(obj->DeletePrivate(priv1));
  CHECK(!obj->HasPrivate(priv1));

  CHECK(obj->SetPrivate(priv1, v8::Integer::New(isolate, 1503)));
  CHECK(obj->HasPrivate(priv1));
  CHECK_EQ(1503, obj->GetPrivate(priv1)->Int32Value());
  CHECK(obj->SetPrivate(priv1, v8::Integer::New(isolate, 2002)));
  CHECK(obj->HasPrivate(priv1));
  CHECK_EQ(2002, obj->GetPrivate(priv1)->Int32Value());

  CHECK_EQ(0u, obj->GetOwnPropertyNames()->Length());
  unsigned num_props = obj->GetPropertyNames()->Length();
  CHECK(obj->Set(v8::String::NewFromUtf8(isolate, "bla"),
                 v8::Integer::New(isolate, 20)));
  CHECK_EQ(1u, obj->GetOwnPropertyNames()->Length());
  CHECK_EQ(num_props + 1, obj->GetPropertyNames()->Length());

  CcTest::heap()->CollectAllGarbage(i::Heap::kNoGCFlags);

  // Add another property and delete it afterwards to force the object in
  // slow case.
  CHECK(obj->SetPrivate(priv2, v8::Integer::New(isolate, 2008)));
  CHECK_EQ(2002, obj->GetPrivate(priv1)->Int32Value());
  CHECK_EQ(2008, obj->GetPrivate(priv2)->Int32Value());
  CHECK_EQ(2002, obj->GetPrivate(priv1)->Int32Value());
  CHECK_EQ(1u, obj->GetOwnPropertyNames()->Length());

  CHECK(obj->HasPrivate(priv1));
  CHECK(obj->HasPrivate(priv2));
  CHECK(obj->DeletePrivate(priv2));
  CHECK(obj->HasPrivate(priv1));
  CHECK(!obj->HasPrivate(priv2));
  CHECK_EQ(2002, obj->GetPrivate(priv1)->Int32Value());
  CHECK_EQ(1u, obj->GetOwnPropertyNames()->Length());

  // Private properties are inherited (for the time being).
  v8::Local<v8::Object> child = v8::Object::New(isolate);
  child->SetPrototype(obj);
  CHECK(child->HasPrivate(priv1));
  CHECK_EQ(2002, child->GetPrivate(priv1)->Int32Value());
  CHECK_EQ(0u, child->GetOwnPropertyNames()->Length());
}


THREADED_TEST(GlobalSymbols) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);

  v8::Local<String> name = v8_str("my-symbol");
  v8::Local<v8::Symbol> glob = v8::Symbol::For(isolate, name);
  v8::Local<v8::Symbol> glob2 = v8::Symbol::For(isolate, name);
  CHECK(glob2->SameValue(glob));

  v8::Local<v8::Symbol> glob_api = v8::Symbol::ForApi(isolate, name);
  v8::Local<v8::Symbol> glob_api2 = v8::Symbol::ForApi(isolate, name);
  CHECK(glob_api2->SameValue(glob_api));
  CHECK(!glob_api->SameValue(glob));

  v8::Local<v8::Symbol> sym = v8::Symbol::New(isolate, name);
  CHECK(!sym->SameValue(glob));

  CompileRun("var sym2 = Symbol.for('my-symbol')");
  v8::Local<Value> sym2 = env->Global()->Get(v8_str("sym2"));
  CHECK(sym2->SameValue(glob));
  CHECK(!sym2->SameValue(glob_api));
}


static void CheckWellKnownSymbol(v8::Local<v8::Symbol>(*getter)(v8::Isolate*),
                                 const char* name) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);

  v8::Local<v8::Symbol> symbol = getter(isolate);
  std::string script = std::string("var sym = ") + name;
  CompileRun(script.c_str());
  v8::Local<Value> value = env->Global()->Get(v8_str("sym"));

  CHECK(!value.IsEmpty());
  CHECK(!symbol.IsEmpty());
  CHECK(value->SameValue(symbol));
}


THREADED_TEST(WellKnownSymbols) {
  CheckWellKnownSymbol(v8::Symbol::GetIterator, "Symbol.iterator");
  CheckWellKnownSymbol(v8::Symbol::GetUnscopables, "Symbol.unscopables");
}


THREADED_TEST(GlobalPrivates) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);

  v8::Local<String> name = v8_str("my-private");
  v8::Local<v8::Private> glob = v8::Private::ForApi(isolate, name);
  v8::Local<v8::Object> obj = v8::Object::New(isolate);
  CHECK(obj->SetPrivate(glob, v8::Integer::New(isolate, 3)));

  v8::Local<v8::Private> glob2 = v8::Private::ForApi(isolate, name);
  CHECK(obj->HasPrivate(glob2));

  v8::Local<v8::Private> priv = v8::Private::New(isolate, name);
  CHECK(!obj->HasPrivate(priv));

  CompileRun("var intern = %CreateGlobalPrivateSymbol('my-private')");
  v8::Local<Value> intern = env->Global()->Get(v8_str("intern"));
  CHECK(!obj->Has(intern));
}


class ScopedArrayBufferContents {
 public:
  explicit ScopedArrayBufferContents(const v8::ArrayBuffer::Contents& contents)
      : contents_(contents) {}
  ~ScopedArrayBufferContents() { free(contents_.Data()); }
  void* Data() const { return contents_.Data(); }
  size_t ByteLength() const { return contents_.ByteLength(); }

 private:
  const v8::ArrayBuffer::Contents contents_;
};

template <typename T>
static void CheckInternalFieldsAreZero(v8::Handle<T> value) {
  CHECK_EQ(T::kInternalFieldCount, value->InternalFieldCount());
  for (int i = 0; i < value->InternalFieldCount(); i++) {
    CHECK_EQ(0, value->GetInternalField(i)->Int32Value());
  }
}


THREADED_TEST(ArrayBuffer_ApiInternalToExternal) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope handle_scope(isolate);

  Local<v8::ArrayBuffer> ab = v8::ArrayBuffer::New(isolate, 1024);
  CheckInternalFieldsAreZero(ab);
  CHECK_EQ(1024, static_cast<int>(ab->ByteLength()));
  CHECK(!ab->IsExternal());
  CcTest::heap()->CollectAllGarbage(i::Heap::kNoGCFlags);

  ScopedArrayBufferContents ab_contents(ab->Externalize());
  CHECK(ab->IsExternal());

  CHECK_EQ(1024, static_cast<int>(ab_contents.ByteLength()));
  uint8_t* data = static_cast<uint8_t*>(ab_contents.Data());
  DCHECK(data != NULL);
  env->Global()->Set(v8_str("ab"), ab);

  v8::Handle<v8::Value> result = CompileRun("ab.byteLength");
  CHECK_EQ(1024, result->Int32Value());

  result = CompileRun(
      "var u8 = new Uint8Array(ab);"
      "u8[0] = 0xFF;"
      "u8[1] = 0xAA;"
      "u8.length");
  CHECK_EQ(1024, result->Int32Value());
  CHECK_EQ(0xFF, data[0]);
  CHECK_EQ(0xAA, data[1]);
  data[0] = 0xCC;
  data[1] = 0x11;
  result = CompileRun("u8[0] + u8[1]");
  CHECK_EQ(0xDD, result->Int32Value());
}


THREADED_TEST(ArrayBuffer_JSInternalToExternal) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope handle_scope(isolate);


  v8::Local<v8::Value> result = CompileRun(
      "var ab1 = new ArrayBuffer(2);"
      "var u8_a = new Uint8Array(ab1);"
      "u8_a[0] = 0xAA;"
      "u8_a[1] = 0xFF; u8_a.buffer");
  Local<v8::ArrayBuffer> ab1 = Local<v8::ArrayBuffer>::Cast(result);
  CheckInternalFieldsAreZero(ab1);
  CHECK_EQ(2, static_cast<int>(ab1->ByteLength()));
  CHECK(!ab1->IsExternal());
  ScopedArrayBufferContents ab1_contents(ab1->Externalize());
  CHECK(ab1->IsExternal());

  result = CompileRun("ab1.byteLength");
  CHECK_EQ(2, result->Int32Value());
  result = CompileRun("u8_a[0]");
  CHECK_EQ(0xAA, result->Int32Value());
  result = CompileRun("u8_a[1]");
  CHECK_EQ(0xFF, result->Int32Value());
  result = CompileRun(
      "var u8_b = new Uint8Array(ab1);"
      "u8_b[0] = 0xBB;"
      "u8_a[0]");
  CHECK_EQ(0xBB, result->Int32Value());
  result = CompileRun("u8_b[1]");
  CHECK_EQ(0xFF, result->Int32Value());

  CHECK_EQ(2, static_cast<int>(ab1_contents.ByteLength()));
  uint8_t* ab1_data = static_cast<uint8_t*>(ab1_contents.Data());
  CHECK_EQ(0xBB, ab1_data[0]);
  CHECK_EQ(0xFF, ab1_data[1]);
  ab1_data[0] = 0xCC;
  ab1_data[1] = 0x11;
  result = CompileRun("u8_a[0] + u8_a[1]");
  CHECK_EQ(0xDD, result->Int32Value());
}


THREADED_TEST(ArrayBuffer_External) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope handle_scope(isolate);

  i::ScopedVector<uint8_t> my_data(100);
  memset(my_data.start(), 0, 100);
  Local<v8::ArrayBuffer> ab3 =
      v8::ArrayBuffer::New(isolate, my_data.start(), 100);
  CheckInternalFieldsAreZero(ab3);
  CHECK_EQ(100, static_cast<int>(ab3->ByteLength()));
  CHECK(ab3->IsExternal());

  env->Global()->Set(v8_str("ab3"), ab3);

  v8::Handle<v8::Value> result = CompileRun("ab3.byteLength");
  CHECK_EQ(100, result->Int32Value());

  result = CompileRun(
      "var u8_b = new Uint8Array(ab3);"
      "u8_b[0] = 0xBB;"
      "u8_b[1] = 0xCC;"
      "u8_b.length");
  CHECK_EQ(100, result->Int32Value());
  CHECK_EQ(0xBB, my_data[0]);
  CHECK_EQ(0xCC, my_data[1]);
  my_data[0] = 0xCC;
  my_data[1] = 0x11;
  result = CompileRun("u8_b[0] + u8_b[1]");
  CHECK_EQ(0xDD, result->Int32Value());
}


THREADED_TEST(ArrayBuffer_DisableNeuter) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope handle_scope(isolate);

  i::ScopedVector<uint8_t> my_data(100);
  memset(my_data.start(), 0, 100);
  Local<v8::ArrayBuffer> ab =
      v8::ArrayBuffer::New(isolate, my_data.start(), 100);
  CHECK(ab->IsNeuterable());

  i::Handle<i::JSArrayBuffer> buf = v8::Utils::OpenHandle(*ab);
  buf->set_is_neuterable(false);

  CHECK(!ab->IsNeuterable());
}


static void CheckDataViewIsNeutered(v8::Handle<v8::DataView> dv) {
  CHECK_EQ(0, static_cast<int>(dv->ByteLength()));
  CHECK_EQ(0, static_cast<int>(dv->ByteOffset()));
}


static void CheckIsNeutered(v8::Handle<v8::TypedArray> ta) {
  CHECK_EQ(0, static_cast<int>(ta->ByteLength()));
  CHECK_EQ(0, static_cast<int>(ta->Length()));
  CHECK_EQ(0, static_cast<int>(ta->ByteOffset()));
}


static void CheckIsTypedArrayVarNeutered(const char* name) {
  i::ScopedVector<char> source(1024);
  i::SNPrintF(source,
              "%s.byteLength == 0 && %s.byteOffset == 0 && %s.length == 0",
              name, name, name);
  CHECK(CompileRun(source.start())->IsTrue());
  v8::Handle<v8::TypedArray> ta =
      v8::Handle<v8::TypedArray>::Cast(CompileRun(name));
  CheckIsNeutered(ta);
}


template <typename TypedArray, int kElementSize>
static Handle<TypedArray> CreateAndCheck(Handle<v8::ArrayBuffer> ab,
                                         int byteOffset, int length) {
  v8::Handle<TypedArray> ta = TypedArray::New(ab, byteOffset, length);
  CheckInternalFieldsAreZero<v8::ArrayBufferView>(ta);
  CHECK_EQ(byteOffset, static_cast<int>(ta->ByteOffset()));
  CHECK_EQ(length, static_cast<int>(ta->Length()));
  CHECK_EQ(length * kElementSize, static_cast<int>(ta->ByteLength()));
  return ta;
}


THREADED_TEST(ArrayBuffer_NeuteringApi) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope handle_scope(isolate);

  v8::Handle<v8::ArrayBuffer> buffer = v8::ArrayBuffer::New(isolate, 1024);

  v8::Handle<v8::Uint8Array> u8a =
      CreateAndCheck<v8::Uint8Array, 1>(buffer, 1, 1023);
  v8::Handle<v8::Uint8ClampedArray> u8c =
      CreateAndCheck<v8::Uint8ClampedArray, 1>(buffer, 1, 1023);
  v8::Handle<v8::Int8Array> i8a =
      CreateAndCheck<v8::Int8Array, 1>(buffer, 1, 1023);

  v8::Handle<v8::Uint16Array> u16a =
      CreateAndCheck<v8::Uint16Array, 2>(buffer, 2, 511);
  v8::Handle<v8::Int16Array> i16a =
      CreateAndCheck<v8::Int16Array, 2>(buffer, 2, 511);

  v8::Handle<v8::Uint32Array> u32a =
      CreateAndCheck<v8::Uint32Array, 4>(buffer, 4, 255);
  v8::Handle<v8::Int32Array> i32a =
      CreateAndCheck<v8::Int32Array, 4>(buffer, 4, 255);

  v8::Handle<v8::Float32Array> f32a =
      CreateAndCheck<v8::Float32Array, 4>(buffer, 4, 255);
  v8::Handle<v8::Float64Array> f64a =
      CreateAndCheck<v8::Float64Array, 8>(buffer, 8, 127);

  v8::Handle<v8::DataView> dv = v8::DataView::New(buffer, 1, 1023);
  CheckInternalFieldsAreZero<v8::ArrayBufferView>(dv);
  CHECK_EQ(1, static_cast<int>(dv->ByteOffset()));
  CHECK_EQ(1023, static_cast<int>(dv->ByteLength()));

  ScopedArrayBufferContents contents(buffer->Externalize());
  buffer->Neuter();
  CHECK_EQ(0, static_cast<int>(buffer->ByteLength()));
  CheckIsNeutered(u8a);
  CheckIsNeutered(u8c);
  CheckIsNeutered(i8a);
  CheckIsNeutered(u16a);
  CheckIsNeutered(i16a);
  CheckIsNeutered(u32a);
  CheckIsNeutered(i32a);
  CheckIsNeutered(f32a);
  CheckIsNeutered(f64a);
  CheckDataViewIsNeutered(dv);
}


THREADED_TEST(ArrayBuffer_NeuteringScript) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope handle_scope(isolate);

  CompileRun(
      "var ab = new ArrayBuffer(1024);"
      "var u8a = new Uint8Array(ab, 1, 1023);"
      "var u8c = new Uint8ClampedArray(ab, 1, 1023);"
      "var i8a = new Int8Array(ab, 1, 1023);"
      "var u16a = new Uint16Array(ab, 2, 511);"
      "var i16a = new Int16Array(ab, 2, 511);"
      "var u32a = new Uint32Array(ab, 4, 255);"
      "var i32a = new Int32Array(ab, 4, 255);"
      "var f32a = new Float32Array(ab, 4, 255);"
      "var f64a = new Float64Array(ab, 8, 127);"
      "var dv = new DataView(ab, 1, 1023);");

  v8::Handle<v8::ArrayBuffer> ab =
      Local<v8::ArrayBuffer>::Cast(CompileRun("ab"));

  v8::Handle<v8::DataView> dv =
      v8::Handle<v8::DataView>::Cast(CompileRun("dv"));

  ScopedArrayBufferContents contents(ab->Externalize());
  ab->Neuter();
  CHECK_EQ(0, static_cast<int>(ab->ByteLength()));
  CHECK_EQ(0, CompileRun("ab.byteLength")->Int32Value());

  CheckIsTypedArrayVarNeutered("u8a");
  CheckIsTypedArrayVarNeutered("u8c");
  CheckIsTypedArrayVarNeutered("i8a");
  CheckIsTypedArrayVarNeutered("u16a");
  CheckIsTypedArrayVarNeutered("i16a");
  CheckIsTypedArrayVarNeutered("u32a");
  CheckIsTypedArrayVarNeutered("i32a");
  CheckIsTypedArrayVarNeutered("f32a");
  CheckIsTypedArrayVarNeutered("f64a");

  CHECK(CompileRun("dv.byteLength == 0 && dv.byteOffset == 0")->IsTrue());
  CheckDataViewIsNeutered(dv);
}


THREADED_TEST(HiddenProperties) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);

  v8::Local<v8::Object> obj = v8::Object::New(env->GetIsolate());
  v8::Local<v8::String> key = v8_str("api-test::hidden-key");
  v8::Local<v8::String> empty = v8_str("");
  v8::Local<v8::String> prop_name = v8_str("prop_name");

  CcTest::heap()->CollectAllGarbage(i::Heap::kNoGCFlags);

  // Make sure delete of a non-existent hidden value works
  CHECK(obj->DeleteHiddenValue(key));

  CHECK(obj->SetHiddenValue(key, v8::Integer::New(isolate, 1503)));
  CHECK_EQ(1503, obj->GetHiddenValue(key)->Int32Value());
  CHECK(obj->SetHiddenValue(key, v8::Integer::New(isolate, 2002)));
  CHECK_EQ(2002, obj->GetHiddenValue(key)->Int32Value());

  CcTest::heap()->CollectAllGarbage(i::Heap::kNoGCFlags);

  // Make sure we do not find the hidden property.
  CHECK(!obj->Has(empty));
  CHECK_EQ(2002, obj->GetHiddenValue(key)->Int32Value());
  CHECK(obj->Get(empty)->IsUndefined());
  CHECK_EQ(2002, obj->GetHiddenValue(key)->Int32Value());
  CHECK(obj->Set(empty, v8::Integer::New(isolate, 2003)));
  CHECK_EQ(2002, obj->GetHiddenValue(key)->Int32Value());
  CHECK_EQ(2003, obj->Get(empty)->Int32Value());

  CcTest::heap()->CollectAllGarbage(i::Heap::kNoGCFlags);

  // Add another property and delete it afterwards to force the object in
  // slow case.
  CHECK(obj->Set(prop_name, v8::Integer::New(isolate, 2008)));
  CHECK_EQ(2002, obj->GetHiddenValue(key)->Int32Value());
  CHECK_EQ(2008, obj->Get(prop_name)->Int32Value());
  CHECK_EQ(2002, obj->GetHiddenValue(key)->Int32Value());
  CHECK(obj->Delete(prop_name));
  CHECK_EQ(2002, obj->GetHiddenValue(key)->Int32Value());

  CcTest::heap()->CollectAllGarbage(i::Heap::kNoGCFlags);

  CHECK(obj->SetHiddenValue(key, Handle<Value>()));
  CHECK(obj->GetHiddenValue(key).IsEmpty());

  CHECK(obj->SetHiddenValue(key, v8::Integer::New(isolate, 2002)));
  CHECK(obj->DeleteHiddenValue(key));
  CHECK(obj->GetHiddenValue(key).IsEmpty());
}


THREADED_TEST(Regress97784) {
  // Regression test for crbug.com/97784
  // Messing with the Object.prototype should not have effect on
  // hidden properties.
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());

  v8::Local<v8::Object> obj = v8::Object::New(env->GetIsolate());
  v8::Local<v8::String> key = v8_str("hidden");

  CompileRun(
      "set_called = false;"
      "Object.defineProperty("
      "    Object.prototype,"
      "    'hidden',"
      "    {get: function() { return 45; },"
      "     set: function() { set_called = true; }})");

  CHECK(obj->GetHiddenValue(key).IsEmpty());
  // Make sure that the getter and setter from Object.prototype is not invoked.
  // If it did we would have full access to the hidden properties in
  // the accessor.
  CHECK(obj->SetHiddenValue(key, v8::Integer::New(env->GetIsolate(), 42)));
  ExpectFalse("set_called");
  CHECK_EQ(42, obj->GetHiddenValue(key)->Int32Value());
}


THREADED_TEST(External) {
  v8::HandleScope scope(CcTest::isolate());
  int x = 3;
  Local<v8::External> ext = v8::External::New(CcTest::isolate(), &x);
  LocalContext env;
  env->Global()->Set(v8_str("ext"), ext);
  Local<Value> reext_obj = CompileRun("this.ext");
  v8::Handle<v8::External> reext = reext_obj.As<v8::External>();
  int* ptr = static_cast<int*>(reext->Value());
  CHECK_EQ(x, 3);
  *ptr = 10;
  CHECK_EQ(x, 10);

  // Make sure unaligned pointers are wrapped properly.
  char* data = i::StrDup("0123456789");
  Local<v8::Value> zero = v8::External::New(CcTest::isolate(), &data[0]);
  Local<v8::Value> one = v8::External::New(CcTest::isolate(), &data[1]);
  Local<v8::Value> two = v8::External::New(CcTest::isolate(), &data[2]);
  Local<v8::Value> three = v8::External::New(CcTest::isolate(), &data[3]);

  char* char_ptr = reinterpret_cast<char*>(v8::External::Cast(*zero)->Value());
  CHECK_EQ('0', *char_ptr);
  char_ptr = reinterpret_cast<char*>(v8::External::Cast(*one)->Value());
  CHECK_EQ('1', *char_ptr);
  char_ptr = reinterpret_cast<char*>(v8::External::Cast(*two)->Value());
  CHECK_EQ('2', *char_ptr);
  char_ptr = reinterpret_cast<char*>(v8::External::Cast(*three)->Value());
  CHECK_EQ('3', *char_ptr);
  i::DeleteArray(data);
}


THREADED_TEST(GlobalHandle) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::Persistent<String> global;
  {
    v8::HandleScope scope(isolate);
    global.Reset(isolate, v8_str("str"));
  }
  {
    v8::HandleScope scope(isolate);
    CHECK_EQ(v8::Local<String>::New(isolate, global)->Length(), 3);
  }
  global.Reset();
  {
    v8::HandleScope scope(isolate);
    global.Reset(isolate, v8_str("str"));
  }
  {
    v8::HandleScope scope(isolate);
    CHECK_EQ(v8::Local<String>::New(isolate, global)->Length(), 3);
  }
  global.Reset();
}


THREADED_TEST(ResettingGlobalHandle) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::Persistent<String> global;
  {
    v8::HandleScope scope(isolate);
    global.Reset(isolate, v8_str("str"));
  }
  v8::internal::GlobalHandles* global_handles =
      reinterpret_cast<v8::internal::Isolate*>(isolate)->global_handles();
  int initial_handle_count = global_handles->global_handles_count();
  {
    v8::HandleScope scope(isolate);
    CHECK_EQ(v8::Local<String>::New(isolate, global)->Length(), 3);
  }
  {
    v8::HandleScope scope(isolate);
    global.Reset(isolate, v8_str("longer"));
  }
  CHECK_EQ(global_handles->global_handles_count(), initial_handle_count);
  {
    v8::HandleScope scope(isolate);
    CHECK_EQ(v8::Local<String>::New(isolate, global)->Length(), 6);
  }
  global.Reset();
  CHECK_EQ(global_handles->global_handles_count(), initial_handle_count - 1);
}


THREADED_TEST(ResettingGlobalHandleToEmpty) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::Persistent<String> global;
  {
    v8::HandleScope scope(isolate);
    global.Reset(isolate, v8_str("str"));
  }
  v8::internal::GlobalHandles* global_handles =
      reinterpret_cast<v8::internal::Isolate*>(isolate)->global_handles();
  int initial_handle_count = global_handles->global_handles_count();
  {
    v8::HandleScope scope(isolate);
    CHECK_EQ(v8::Local<String>::New(isolate, global)->Length(), 3);
  }
  {
    v8::HandleScope scope(isolate);
    Local<String> empty;
    global.Reset(isolate, empty);
  }
  CHECK(global.IsEmpty());
  CHECK_EQ(global_handles->global_handles_count(), initial_handle_count - 1);
}


template <class T>
static v8::Global<T> PassUnique(v8::Global<T> unique) {
  return unique.Pass();
}


template <class T>
static v8::Global<T> ReturnUnique(v8::Isolate* isolate,
                                  const v8::Persistent<T>& global) {
  v8::Global<String> unique(isolate, global);
  return unique.Pass();
}


THREADED_TEST(Global) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::Persistent<String> global;
  {
    v8::HandleScope scope(isolate);
    global.Reset(isolate, v8_str("str"));
  }
  v8::internal::GlobalHandles* global_handles =
      reinterpret_cast<v8::internal::Isolate*>(isolate)->global_handles();
  int initial_handle_count = global_handles->global_handles_count();
  {
    v8::Global<String> unique(isolate, global);
    CHECK_EQ(initial_handle_count + 1, global_handles->global_handles_count());
    // Test assignment via Pass
    {
      v8::Global<String> copy = unique.Pass();
      CHECK(unique.IsEmpty());
      CHECK(copy == global);
      CHECK_EQ(initial_handle_count + 1,
               global_handles->global_handles_count());
      unique = copy.Pass();
    }
    // Test ctor via Pass
    {
      v8::Global<String> copy(unique.Pass());
      CHECK(unique.IsEmpty());
      CHECK(copy == global);
      CHECK_EQ(initial_handle_count + 1,
               global_handles->global_handles_count());
      unique = copy.Pass();
    }
    // Test pass through function call
    {
      v8::Global<String> copy = PassUnique(unique.Pass());
      CHECK(unique.IsEmpty());
      CHECK(copy == global);
      CHECK_EQ(initial_handle_count + 1,
               global_handles->global_handles_count());
      unique = copy.Pass();
    }
    CHECK_EQ(initial_handle_count + 1, global_handles->global_handles_count());
  }
  // Test pass from function call
  {
    v8::Global<String> unique = ReturnUnique(isolate, global);
    CHECK(unique == global);
    CHECK_EQ(initial_handle_count + 1, global_handles->global_handles_count());
  }
  CHECK_EQ(initial_handle_count, global_handles->global_handles_count());
  global.Reset();
}


namespace {

class TwoPassCallbackData;
void FirstPassCallback(const v8::WeakCallbackInfo<TwoPassCallbackData>& data);
void SecondPassCallback(const v8::WeakCallbackInfo<TwoPassCallbackData>& data);


class TwoPassCallbackData {
 public:
  TwoPassCallbackData(v8::Isolate* isolate, int* instance_counter)
      : first_pass_called_(false),
        second_pass_called_(false),
        trigger_gc_(false),
        instance_counter_(instance_counter) {
    HandleScope scope(isolate);
    i::ScopedVector<char> buffer(40);
    i::SNPrintF(buffer, "%p", static_cast<void*>(this));
    auto string =
        v8::String::NewFromUtf8(isolate, buffer.start(),
                                v8::NewStringType::kNormal).ToLocalChecked();
    cell_.Reset(isolate, string);
    (*instance_counter_)++;
  }

  ~TwoPassCallbackData() {
    CHECK(first_pass_called_);
    CHECK(second_pass_called_);
    CHECK(cell_.IsEmpty());
    (*instance_counter_)--;
  }

  void FirstPass() {
    CHECK(!first_pass_called_);
    CHECK(!second_pass_called_);
    CHECK(!cell_.IsEmpty());
    cell_.Reset();
    first_pass_called_ = true;
  }

  void SecondPass() {
    CHECK(first_pass_called_);
    CHECK(!second_pass_called_);
    CHECK(cell_.IsEmpty());
    second_pass_called_ = true;
    delete this;
  }

  void SetWeak() {
    cell_.SetWeak(this, FirstPassCallback, v8::WeakCallbackType::kParameter);
  }

  void MarkTriggerGc() { trigger_gc_ = true; }
  bool trigger_gc() { return trigger_gc_; }

  int* instance_counter() { return instance_counter_; }

 private:
  bool first_pass_called_;
  bool second_pass_called_;
  bool trigger_gc_;
  v8::Global<v8::String> cell_;
  int* instance_counter_;
};


void SecondPassCallback(const v8::WeakCallbackInfo<TwoPassCallbackData>& data) {
  ApiTestFuzzer::Fuzz();
  bool trigger_gc = data.GetParameter()->trigger_gc();
  int* instance_counter = data.GetParameter()->instance_counter();
  data.GetParameter()->SecondPass();
  if (!trigger_gc) return;
  auto data_2 = new TwoPassCallbackData(data.GetIsolate(), instance_counter);
  data_2->SetWeak();
  CcTest::heap()->CollectAllGarbage(i::Heap::kAbortIncrementalMarkingMask);
}


void FirstPassCallback(const v8::WeakCallbackInfo<TwoPassCallbackData>& data) {
  data.GetParameter()->FirstPass();
  data.SetSecondPassCallback(SecondPassCallback);
}

}  // namespace


TEST(TwoPassPhantomCallbacks) {
  auto isolate = CcTest::isolate();
  const size_t kLength = 20;
  int instance_counter = 0;
  for (size_t i = 0; i < kLength; ++i) {
    auto data = new TwoPassCallbackData(isolate, &instance_counter);
    data->SetWeak();
  }
  CHECK_EQ(static_cast<int>(kLength), instance_counter);
  CcTest::heap()->CollectAllGarbage(i::Heap::kAbortIncrementalMarkingMask);
  CHECK_EQ(0, instance_counter);
}


TEST(TwoPassPhantomCallbacksNestedGc) {
  auto isolate = CcTest::isolate();
  const size_t kLength = 20;
  TwoPassCallbackData* array[kLength];
  int instance_counter = 0;
  for (size_t i = 0; i < kLength; ++i) {
    array[i] = new TwoPassCallbackData(isolate, &instance_counter);
    array[i]->SetWeak();
  }
  array[5]->MarkTriggerGc();
  array[10]->MarkTriggerGc();
  array[15]->MarkTriggerGc();
  CHECK_EQ(static_cast<int>(kLength), instance_counter);
  CcTest::heap()->CollectAllGarbage(i::Heap::kAbortIncrementalMarkingMask);
  CHECK_EQ(0, instance_counter);
}


template <typename K, typename V>
class WeakStdMapTraits : public v8::StdMapTraits<K, V> {
 public:
  typedef typename v8::PersistentValueMap<K, V, WeakStdMapTraits<K, V>> MapType;
  static const v8::PersistentContainerCallbackType kCallbackType = v8::kWeak;
  struct WeakCallbackDataType {
    MapType* map;
    K key;
  };
  static WeakCallbackDataType* WeakCallbackParameter(MapType* map, const K& key,
                                                     Local<V> value) {
    WeakCallbackDataType* data = new WeakCallbackDataType;
    data->map = map;
    data->key = key;
    return data;
  }
  static MapType* MapFromWeakCallbackData(
      const v8::WeakCallbackData<V, WeakCallbackDataType>& data) {
    return data.GetParameter()->map;
  }
  static K KeyFromWeakCallbackData(
      const v8::WeakCallbackData<V, WeakCallbackDataType>& data) {
    return data.GetParameter()->key;
  }
  static void DisposeCallbackData(WeakCallbackDataType* data) { delete data; }
  static void Dispose(v8::Isolate* isolate, v8::Global<V> value, K key) {}
};


template <typename Map>
static void TestPersistentValueMap() {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  Map map(isolate);
  v8::internal::GlobalHandles* global_handles =
      reinterpret_cast<v8::internal::Isolate*>(isolate)->global_handles();
  int initial_handle_count = global_handles->global_handles_count();
  CHECK_EQ(0, static_cast<int>(map.Size()));
  {
    HandleScope scope(isolate);
    Local<v8::Object> obj = map.Get(7);
    CHECK(obj.IsEmpty());
    Local<v8::Object> expected = v8::Object::New(isolate);
    map.Set(7, expected);
    CHECK_EQ(1, static_cast<int>(map.Size()));
    obj = map.Get(7);
    CHECK(expected->Equals(obj));
    {
      typename Map::PersistentValueReference ref = map.GetReference(7);
      CHECK(expected->Equals(ref.NewLocal(isolate)));
    }
    v8::Global<v8::Object> removed = map.Remove(7);
    CHECK_EQ(0, static_cast<int>(map.Size()));
    CHECK(expected == removed);
    removed = map.Remove(7);
    CHECK(removed.IsEmpty());
    map.Set(8, expected);
    CHECK_EQ(1, static_cast<int>(map.Size()));
    map.Set(8, expected);
    CHECK_EQ(1, static_cast<int>(map.Size()));
    {
      typename Map::PersistentValueReference ref;
      Local<v8::Object> expected2 = v8::Object::New(isolate);
      removed = map.Set(8, v8::Global<v8::Object>(isolate, expected2), &ref);
      CHECK_EQ(1, static_cast<int>(map.Size()));
      CHECK(expected == removed);
      CHECK(expected2->Equals(ref.NewLocal(isolate)));
    }
  }
  CHECK_EQ(initial_handle_count + 1, global_handles->global_handles_count());
  if (map.IsWeak()) {
    reinterpret_cast<v8::internal::Isolate*>(isolate)
        ->heap()
        ->CollectAllGarbage(i::Heap::kAbortIncrementalMarkingMask);
  } else {
    map.Clear();
  }
  CHECK_EQ(0, static_cast<int>(map.Size()));
  CHECK_EQ(initial_handle_count, global_handles->global_handles_count());
}


TEST(PersistentValueMap) {
  // Default case, w/o weak callbacks:
  TestPersistentValueMap<v8::StdPersistentValueMap<int, v8::Object>>();

  // Custom traits with weak callbacks:
  typedef v8::PersistentValueMap<int, v8::Object,
                                 WeakStdMapTraits<int, v8::Object>>
      WeakPersistentValueMap;
  TestPersistentValueMap<WeakPersistentValueMap>();
}


namespace {

void* IntKeyToVoidPointer(int key) { return reinterpret_cast<void*>(key << 1); }


Local<v8::Object> NewObjectForIntKey(
    v8::Isolate* isolate, const v8::Global<v8::ObjectTemplate>& templ,
    int key) {
  auto local = Local<v8::ObjectTemplate>::New(isolate, templ);
  auto obj = local->NewInstance();
  obj->SetAlignedPointerInInternalField(0, IntKeyToVoidPointer(key));
  return obj;
}


template <typename K, typename V>
class PhantomStdMapTraits : public v8::StdMapTraits<K, V> {
 public:
  typedef typename v8::GlobalValueMap<K, V, PhantomStdMapTraits<K, V>> MapType;
  static const v8::PersistentContainerCallbackType kCallbackType =
      v8::kWeakWithInternalFields;
  struct WeakCallbackDataType {
    MapType* map;
    K key;
  };
  static WeakCallbackDataType* WeakCallbackParameter(MapType* map, const K& key,
                                                     Local<V> value) {
    WeakCallbackDataType* data = new WeakCallbackDataType;
    data->map = map;
    data->key = key;
    return data;
  }
  static MapType* MapFromWeakCallbackInfo(
      const v8::WeakCallbackInfo<WeakCallbackDataType>& data) {
    return data.GetParameter()->map;
  }
  static K KeyFromWeakCallbackInfo(
      const v8::WeakCallbackInfo<WeakCallbackDataType>& data) {
    return data.GetParameter()->key;
  }
  static void DisposeCallbackData(WeakCallbackDataType* data) { delete data; }
  static void Dispose(v8::Isolate* isolate, v8::Global<V> value, K key) {
    CHECK_EQ(IntKeyToVoidPointer(key),
             v8::Object::GetAlignedPointerFromInternalField(value, 0));
  }
  static void DisposeWeak(
      v8::Isolate* isolate,
      const v8::WeakCallbackInfo<WeakCallbackDataType>& info, K key) {
    CHECK_EQ(IntKeyToVoidPointer(key), info.GetInternalField(0));
    DisposeCallbackData(info.GetParameter());
  }
};

}  // namespace


TEST(GlobalValueMap) {
  typedef v8::GlobalValueMap<int, v8::Object,
                             PhantomStdMapTraits<int, v8::Object>> Map;
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::Global<ObjectTemplate> templ;
  {
    HandleScope scope(isolate);
    auto t = ObjectTemplate::New(isolate);
    t->SetInternalFieldCount(1);
    templ.Reset(isolate, t);
  }
  Map map(isolate);
  v8::internal::GlobalHandles* global_handles =
      reinterpret_cast<v8::internal::Isolate*>(isolate)->global_handles();
  int initial_handle_count = global_handles->global_handles_count();
  CHECK_EQ(0, static_cast<int>(map.Size()));
  {
    HandleScope scope(isolate);
    Local<v8::Object> obj = map.Get(7);
    CHECK(obj.IsEmpty());
    Local<v8::Object> expected = v8::Object::New(isolate);
    map.Set(7, expected);
    CHECK_EQ(1, static_cast<int>(map.Size()));
    obj = map.Get(7);
    CHECK(expected->Equals(obj));
    {
      Map::PersistentValueReference ref = map.GetReference(7);
      CHECK(expected->Equals(ref.NewLocal(isolate)));
    }
    v8::Global<v8::Object> removed = map.Remove(7);
    CHECK_EQ(0, static_cast<int>(map.Size()));
    CHECK(expected == removed);
    removed = map.Remove(7);
    CHECK(removed.IsEmpty());
    map.Set(8, expected);
    CHECK_EQ(1, static_cast<int>(map.Size()));
    map.Set(8, expected);
    CHECK_EQ(1, static_cast<int>(map.Size()));
    {
      Map::PersistentValueReference ref;
      Local<v8::Object> expected2 = NewObjectForIntKey(isolate, templ, 8);
      removed = map.Set(8, v8::Global<v8::Object>(isolate, expected2), &ref);
      CHECK_EQ(1, static_cast<int>(map.Size()));
      CHECK(expected == removed);
      CHECK(expected2->Equals(ref.NewLocal(isolate)));
    }
  }
  CHECK_EQ(initial_handle_count + 1, global_handles->global_handles_count());
  CcTest::i_isolate()->heap()->CollectAllGarbage(
      i::Heap::kAbortIncrementalMarkingMask);
  CHECK_EQ(0, static_cast<int>(map.Size()));
  CHECK_EQ(initial_handle_count, global_handles->global_handles_count());
  {
    HandleScope scope(isolate);
    Local<v8::Object> value = NewObjectForIntKey(isolate, templ, 9);
    map.Set(9, value);
    map.Clear();
  }
  CHECK_EQ(0, static_cast<int>(map.Size()));
  CHECK_EQ(initial_handle_count, global_handles->global_handles_count());
}


TEST(PersistentValueVector) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::internal::GlobalHandles* global_handles =
      reinterpret_cast<v8::internal::Isolate*>(isolate)->global_handles();
  int handle_count = global_handles->global_handles_count();
  HandleScope scope(isolate);

  v8::PersistentValueVector<v8::Object> vector(isolate);

  Local<v8::Object> obj1 = v8::Object::New(isolate);
  Local<v8::Object> obj2 = v8::Object::New(isolate);
  v8::Global<v8::Object> obj3(isolate, v8::Object::New(isolate));

  CHECK(vector.IsEmpty());
  CHECK_EQ(0, static_cast<int>(vector.Size()));

  vector.ReserveCapacity(3);
  CHECK(vector.IsEmpty());

  vector.Append(obj1);
  vector.Append(obj2);
  vector.Append(obj1);
  vector.Append(obj3.Pass());
  vector.Append(obj1);

  CHECK(!vector.IsEmpty());
  CHECK_EQ(5, static_cast<int>(vector.Size()));
  CHECK(obj3.IsEmpty());
  CHECK(obj1->Equals(vector.Get(0)));
  CHECK(obj1->Equals(vector.Get(2)));
  CHECK(obj1->Equals(vector.Get(4)));
  CHECK(obj2->Equals(vector.Get(1)));

  CHECK_EQ(5 + handle_count, global_handles->global_handles_count());

  vector.Clear();
  CHECK(vector.IsEmpty());
  CHECK_EQ(0, static_cast<int>(vector.Size()));
  CHECK_EQ(handle_count, global_handles->global_handles_count());
}


THREADED_TEST(GlobalHandleUpcast) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  v8::Local<String> local = v8::Local<String>::New(isolate, v8_str("str"));
  v8::Persistent<String> global_string(isolate, local);
  v8::Persistent<Value>& global_value =
      v8::Persistent<Value>::Cast(global_string);
  CHECK(v8::Local<v8::Value>::New(isolate, global_value)->IsString());
  CHECK(global_string == v8::Persistent<String>::Cast(global_value));
  global_string.Reset();
}


THREADED_TEST(HandleEquality) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::Persistent<String> global1;
  v8::Persistent<String> global2;
  {
    v8::HandleScope scope(isolate);
    global1.Reset(isolate, v8_str("str"));
    global2.Reset(isolate, v8_str("str2"));
  }
  CHECK_EQ(global1 == global1, true);
  CHECK_EQ(global1 != global1, false);
  {
    v8::HandleScope scope(isolate);
    Local<String> local1 = Local<String>::New(isolate, global1);
    Local<String> local2 = Local<String>::New(isolate, global2);

    CHECK_EQ(global1 == local1, true);
    CHECK_EQ(global1 != local1, false);
    CHECK_EQ(local1 == global1, true);
    CHECK_EQ(local1 != global1, false);

    CHECK_EQ(global1 == local2, false);
    CHECK_EQ(global1 != local2, true);
    CHECK_EQ(local2 == global1, false);
    CHECK_EQ(local2 != global1, true);

    CHECK_EQ(local1 == local2, false);
    CHECK_EQ(local1 != local2, true);

    Local<String> anotherLocal1 = Local<String>::New(isolate, global1);
    CHECK_EQ(local1 == anotherLocal1, true);
    CHECK_EQ(local1 != anotherLocal1, false);
  }
  global1.Reset();
  global2.Reset();
}


THREADED_TEST(LocalHandle) {
  v8::HandleScope scope(CcTest::isolate());
  v8::Local<String> local =
      v8::Local<String>::New(CcTest::isolate(), v8_str("str"));
  CHECK_EQ(local->Length(), 3);
}


class WeakCallCounter {
 public:
  explicit WeakCallCounter(int id) : id_(id), number_of_weak_calls_(0) {}
  int id() { return id_; }
  void increment() { number_of_weak_calls_++; }
  int NumberOfWeakCalls() { return number_of_weak_calls_; }

 private:
  int id_;
  int number_of_weak_calls_;
};


template <typename T>
struct WeakCallCounterAndPersistent {
  explicit WeakCallCounterAndPersistent(WeakCallCounter* counter)
      : counter(counter) {}
  WeakCallCounter* counter;
  v8::Persistent<T> handle;
};


template <typename T>
static void WeakPointerCallback(
    const v8::WeakCallbackData<T, WeakCallCounterAndPersistent<T>>& data) {
  CHECK_EQ(1234, data.GetParameter()->counter->id());
  data.GetParameter()->counter->increment();
  data.GetParameter()->handle.Reset();
}


template <typename T>
static UniqueId MakeUniqueId(const Persistent<T>& p) {
  return UniqueId(reinterpret_cast<uintptr_t>(*v8::Utils::OpenPersistent(p)));
}


THREADED_TEST(ApiObjectGroups) {
  LocalContext env;
  v8::Isolate* iso = env->GetIsolate();
  HandleScope scope(iso);

  WeakCallCounter counter(1234);

  WeakCallCounterAndPersistent<Value> g1s1(&counter);
  WeakCallCounterAndPersistent<Value> g1s2(&counter);
  WeakCallCounterAndPersistent<Value> g1c1(&counter);
  WeakCallCounterAndPersistent<Value> g2s1(&counter);
  WeakCallCounterAndPersistent<Value> g2s2(&counter);
  WeakCallCounterAndPersistent<Value> g2c1(&counter);

  {
    HandleScope scope(iso);
    g1s1.handle.Reset(iso, Object::New(iso));
    g1s2.handle.Reset(iso, Object::New(iso));
    g1c1.handle.Reset(iso, Object::New(iso));
    g1s1.handle.SetWeak(&g1s1, &WeakPointerCallback);
    g1s2.handle.SetWeak(&g1s2, &WeakPointerCallback);
    g1c1.handle.SetWeak(&g1c1, &WeakPointerCallback);

    g2s1.handle.Reset(iso, Object::New(iso));
    g2s2.handle.Reset(iso, Object::New(iso));
    g2c1.handle.Reset(iso, Object::New(iso));
    g2s1.handle.SetWeak(&g2s1, &WeakPointerCallback);
    g2s2.handle.SetWeak(&g2s2, &WeakPointerCallback);
    g2c1.handle.SetWeak(&g2c1, &WeakPointerCallback);
  }

  WeakCallCounterAndPersistent<Value> root(&counter);
  root.handle.Reset(iso, g1s1.handle);  // make a root.

  // Connect group 1 and 2, make a cycle.
  {
    HandleScope scope(iso);
    CHECK(Local<Object>::New(iso, g1s2.handle.As<Object>())
              ->Set(0, Local<Value>::New(iso, g2s2.handle)));
    CHECK(Local<Object>::New(iso, g2s1.handle.As<Object>())
              ->Set(0, Local<Value>::New(iso, g1s1.handle)));
  }

  {
    UniqueId id1 = MakeUniqueId(g1s1.handle);
    UniqueId id2 = MakeUniqueId(g2s2.handle);
    iso->SetObjectGroupId(g1s1.handle, id1);
    iso->SetObjectGroupId(g1s2.handle, id1);
    iso->SetReferenceFromGroup(id1, g1c1.handle);
    iso->SetObjectGroupId(g2s1.handle, id2);
    iso->SetObjectGroupId(g2s2.handle, id2);
    iso->SetReferenceFromGroup(id2, g2c1.handle);
  }
  // Do a single full GC, ensure incremental marking is stopped.
  v8::internal::Heap* heap =
      reinterpret_cast<v8::internal::Isolate*>(iso)->heap();
  heap->CollectAllGarbage(i::Heap::kAbortIncrementalMarkingMask);

  // All object should be alive.
  CHECK_EQ(0, counter.NumberOfWeakCalls());

  // Weaken the root.
  root.handle.SetWeak(&root, &WeakPointerCallback);
  // But make children strong roots---all the objects (except for children)
  // should be collectable now.
  g1c1.handle.ClearWeak();
  g2c1.handle.ClearWeak();

  // Groups are deleted, rebuild groups.
  {
    UniqueId id1 = MakeUniqueId(g1s1.handle);
    UniqueId id2 = MakeUniqueId(g2s2.handle);
    iso->SetObjectGroupId(g1s1.handle, id1);
    iso->SetObjectGroupId(g1s2.handle, id1);
    iso->SetReferenceFromGroup(id1, g1c1.handle);
    iso->SetObjectGroupId(g2s1.handle, id2);
    iso->SetObjectGroupId(g2s2.handle, id2);
    iso->SetReferenceFromGroup(id2, g2c1.handle);
  }

  heap->CollectAllGarbage(i::Heap::kAbortIncrementalMarkingMask);

  // All objects should be gone. 5 global handles in total.
  CHECK_EQ(5, counter.NumberOfWeakCalls());

  // And now make children weak again and collect them.
  g1c1.handle.SetWeak(&g1c1, &WeakPointerCallback);
  g2c1.handle.SetWeak(&g2c1, &WeakPointerCallback);

  heap->CollectAllGarbage(i::Heap::kAbortIncrementalMarkingMask);
  CHECK_EQ(7, counter.NumberOfWeakCalls());
}


THREADED_TEST(ApiObjectGroupsForSubtypes) {
  LocalContext env;
  v8::Isolate* iso = env->GetIsolate();
  HandleScope scope(iso);

  WeakCallCounter counter(1234);

  WeakCallCounterAndPersistent<Object> g1s1(&counter);
  WeakCallCounterAndPersistent<String> g1s2(&counter);
  WeakCallCounterAndPersistent<String> g1c1(&counter);
  WeakCallCounterAndPersistent<Object> g2s1(&counter);
  WeakCallCounterAndPersistent<String> g2s2(&counter);
  WeakCallCounterAndPersistent<String> g2c1(&counter);

  {
    HandleScope scope(iso);
    g1s1.handle.Reset(iso, Object::New(iso));
    g1s2.handle.Reset(iso, String::NewFromUtf8(iso, "foo1"));
    g1c1.handle.Reset(iso, String::NewFromUtf8(iso, "foo2"));
    g1s1.handle.SetWeak(&g1s1, &WeakPointerCallback);
    g1s2.handle.SetWeak(&g1s2, &WeakPointerCallback);
    g1c1.handle.SetWeak(&g1c1, &WeakPointerCallback);

    g2s1.handle.Reset(iso, Object::New(iso));
    g2s2.handle.Reset(iso, String::NewFromUtf8(iso, "foo3"));
    g2c1.handle.Reset(iso, String::NewFromUtf8(iso, "foo4"));
    g2s1.handle.SetWeak(&g2s1, &WeakPointerCallback);
    g2s2.handle.SetWeak(&g2s2, &WeakPointerCallback);
    g2c1.handle.SetWeak(&g2c1, &WeakPointerCallback);
  }

  WeakCallCounterAndPersistent<Value> root(&counter);
  root.handle.Reset(iso, g1s1.handle);  // make a root.

  // Connect group 1 and 2, make a cycle.
  {
    HandleScope scope(iso);
    CHECK(Local<Object>::New(iso, g1s1.handle)
              ->Set(0, Local<Object>::New(iso, g2s1.handle)));
    CHECK(Local<Object>::New(iso, g2s1.handle)
              ->Set(0, Local<Object>::New(iso, g1s1.handle)));
  }

  {
    UniqueId id1 = MakeUniqueId(g1s1.handle);
    UniqueId id2 = MakeUniqueId(g2s2.handle);
    iso->SetObjectGroupId(g1s1.handle, id1);
    iso->SetObjectGroupId(g1s2.handle, id1);
    iso->SetReference(g1s1.handle, g1c1.handle);
    iso->SetObjectGroupId(g2s1.handle, id2);
    iso->SetObjectGroupId(g2s2.handle, id2);
    iso->SetReferenceFromGroup(id2, g2c1.handle);
  }
  // Do a single full GC, ensure incremental marking is stopped.
  v8::internal::Heap* heap =
      reinterpret_cast<v8::internal::Isolate*>(iso)->heap();
  heap->CollectAllGarbage(i::Heap::kAbortIncrementalMarkingMask);

  // All object should be alive.
  CHECK_EQ(0, counter.NumberOfWeakCalls());

  // Weaken the root.
  root.handle.SetWeak(&root, &WeakPointerCallback);
  // But make children strong roots---all the objects (except for children)
  // should be collectable now.
  g1c1.handle.ClearWeak();
  g2c1.handle.ClearWeak();

  // Groups are deleted, rebuild groups.
  {
    UniqueId id1 = MakeUniqueId(g1s1.handle);
    UniqueId id2 = MakeUniqueId(g2s2.handle);
    iso->SetObjectGroupId(g1s1.handle, id1);
    iso->SetObjectGroupId(g1s2.handle, id1);
    iso->SetReference(g1s1.handle, g1c1.handle);
    iso->SetObjectGroupId(g2s1.handle, id2);
    iso->SetObjectGroupId(g2s2.handle, id2);
    iso->SetReferenceFromGroup(id2, g2c1.handle);
  }

  heap->CollectAllGarbage(i::Heap::kAbortIncrementalMarkingMask);

  // All objects should be gone. 5 global handles in total.
  CHECK_EQ(5, counter.NumberOfWeakCalls());

  // And now make children weak again and collect them.
  g1c1.handle.SetWeak(&g1c1, &WeakPointerCallback);
  g2c1.handle.SetWeak(&g2c1, &WeakPointerCallback);

  heap->CollectAllGarbage(i::Heap::kAbortIncrementalMarkingMask);
  CHECK_EQ(7, counter.NumberOfWeakCalls());
}


THREADED_TEST(ApiObjectGroupsCycle) {
  LocalContext env;
  v8::Isolate* iso = env->GetIsolate();
  HandleScope scope(iso);

  WeakCallCounter counter(1234);

  WeakCallCounterAndPersistent<Value> g1s1(&counter);
  WeakCallCounterAndPersistent<Value> g1s2(&counter);
  WeakCallCounterAndPersistent<Value> g2s1(&counter);
  WeakCallCounterAndPersistent<Value> g2s2(&counter);
  WeakCallCounterAndPersistent<Value> g3s1(&counter);
  WeakCallCounterAndPersistent<Value> g3s2(&counter);
  WeakCallCounterAndPersistent<Value> g4s1(&counter);
  WeakCallCounterAndPersistent<Value> g4s2(&counter);

  {
    HandleScope scope(iso);
    g1s1.handle.Reset(iso, Object::New(iso));
    g1s2.handle.Reset(iso, Object::New(iso));
    g1s1.handle.SetWeak(&g1s1, &WeakPointerCallback);
    g1s2.handle.SetWeak(&g1s2, &WeakPointerCallback);
    CHECK(g1s1.handle.IsWeak());
    CHECK(g1s2.handle.IsWeak());

    g2s1.handle.Reset(iso, Object::New(iso));
    g2s2.handle.Reset(iso, Object::New(iso));
    g2s1.handle.SetWeak(&g2s1, &WeakPointerCallback);
    g2s2.handle.SetWeak(&g2s2, &WeakPointerCallback);
    CHECK(g2s1.handle.IsWeak());
    CHECK(g2s2.handle.IsWeak());

    g3s1.handle.Reset(iso, Object::New(iso));
    g3s2.handle.Reset(iso, Object::New(iso));
    g3s1.handle.SetWeak(&g3s1, &WeakPointerCallback);
    g3s2.handle.SetWeak(&g3s2, &WeakPointerCallback);
    CHECK(g3s1.handle.IsWeak());
    CHECK(g3s2.handle.IsWeak());

    g4s1.handle.Reset(iso, Object::New(iso));
    g4s2.handle.Reset(iso, Object::New(iso));
    g4s1.handle.SetWeak(&g4s1, &WeakPointerCallback);
    g4s2.handle.SetWeak(&g4s2, &WeakPointerCallback);
    CHECK(g4s1.handle.IsWeak());
    CHECK(g4s2.handle.IsWeak());
  }

  WeakCallCounterAndPersistent<Value> root(&counter);
  root.handle.Reset(iso, g1s1.handle);  // make a root.

  // Connect groups.  We're building the following cycle:
  // G1: { g1s1, g2s1 }, g1s1 implicitly references g2s1, ditto for other
  // groups.
  {
    UniqueId id1 = MakeUniqueId(g1s1.handle);
    UniqueId id2 = MakeUniqueId(g2s1.handle);
    UniqueId id3 = MakeUniqueId(g3s1.handle);
    UniqueId id4 = MakeUniqueId(g4s1.handle);
    iso->SetObjectGroupId(g1s1.handle, id1);
    iso->SetObjectGroupId(g1s2.handle, id1);
    iso->SetReferenceFromGroup(id1, g2s1.handle);
    iso->SetObjectGroupId(g2s1.handle, id2);
    iso->SetObjectGroupId(g2s2.handle, id2);
    iso->SetReferenceFromGroup(id2, g3s1.handle);
    iso->SetObjectGroupId(g3s1.handle, id3);
    iso->SetObjectGroupId(g3s2.handle, id3);
    iso->SetReferenceFromGroup(id3, g4s1.handle);
    iso->SetObjectGroupId(g4s1.handle, id4);
    iso->SetObjectGroupId(g4s2.handle, id4);
    iso->SetReferenceFromGroup(id4, g1s1.handle);
  }
  // Do a single full GC
  v8::internal::Heap* heap =
      reinterpret_cast<v8::internal::Isolate*>(iso)->heap();
  heap->CollectAllGarbage(i::Heap::kAbortIncrementalMarkingMask);

  // All object should be alive.
  CHECK_EQ(0, counter.NumberOfWeakCalls());

  // Weaken the root.
  root.handle.SetWeak(&root, &WeakPointerCallback);

  // Groups are deleted, rebuild groups.
  {
    UniqueId id1 = MakeUniqueId(g1s1.handle);
    UniqueId id2 = MakeUniqueId(g2s1.handle);
    UniqueId id3 = MakeUniqueId(g3s1.handle);
    UniqueId id4 = MakeUniqueId(g4s1.handle);
    iso->SetObjectGroupId(g1s1.handle, id1);
    iso->SetObjectGroupId(g1s2.handle, id1);
    iso->SetReferenceFromGroup(id1, g2s1.handle);
    iso->SetObjectGroupId(g2s1.handle, id2);
    iso->SetObjectGroupId(g2s2.handle, id2);
    iso->SetReferenceFromGroup(id2, g3s1.handle);
    iso->SetObjectGroupId(g3s1.handle, id3);
    iso->SetObjectGroupId(g3s2.handle, id3);
    iso->SetReferenceFromGroup(id3, g4s1.handle);
    iso->SetObjectGroupId(g4s1.handle, id4);
    iso->SetObjectGroupId(g4s2.handle, id4);
    iso->SetReferenceFromGroup(id4, g1s1.handle);
  }

  heap->CollectAllGarbage(i::Heap::kAbortIncrementalMarkingMask);

  // All objects should be gone. 9 global handles in total.
  CHECK_EQ(9, counter.NumberOfWeakCalls());
}


THREADED_TEST(WeakRootsSurviveTwoRoundsOfGC) {
  LocalContext env;
  v8::Isolate* iso = env->GetIsolate();
  HandleScope scope(iso);

  WeakCallCounter counter(1234);

  WeakCallCounterAndPersistent<Value> weak_obj(&counter);

  // Create a weak object that references a internalized string.
  {
    HandleScope scope(iso);
    weak_obj.handle.Reset(iso, Object::New(iso));
    weak_obj.handle.SetWeak(&weak_obj, &WeakPointerCallback);
    CHECK(weak_obj.handle.IsWeak());
    Local<Object>::New(iso, weak_obj.handle.As<Object>())
        ->Set(v8_str("x"), String::NewFromUtf8(iso, "magic cookie",
                                               String::kInternalizedString));
  }
  // Do a single full GC
  i::Isolate* i_iso = reinterpret_cast<v8::internal::Isolate*>(iso);
  i::Heap* heap = i_iso->heap();
  heap->CollectAllGarbage(i::Heap::kAbortIncrementalMarkingMask);

  // We should have received the weak callback.
  CHECK_EQ(1, counter.NumberOfWeakCalls());

  // Check that the string is still alive.
  {
    HandleScope scope(iso);
    i::MaybeHandle<i::String> magic_string =
        i::StringTable::LookupStringIfExists(
            i_iso,
            v8::Utils::OpenHandle(*String::NewFromUtf8(iso, "magic cookie")));
    magic_string.Check();
  }
}


// TODO(mstarzinger): This should be a THREADED_TEST but causes failures
// on the buildbots, so was made non-threaded for the time being.
TEST(ApiObjectGroupsCycleForScavenger) {
  i::FLAG_stress_compaction = false;
  i::FLAG_gc_global = false;
  LocalContext env;
  v8::Isolate* iso = env->GetIsolate();
  HandleScope scope(iso);

  WeakCallCounter counter(1234);

  WeakCallCounterAndPersistent<Value> g1s1(&counter);
  WeakCallCounterAndPersistent<Value> g1s2(&counter);
  WeakCallCounterAndPersistent<Value> g2s1(&counter);
  WeakCallCounterAndPersistent<Value> g2s2(&counter);
  WeakCallCounterAndPersistent<Value> g3s1(&counter);
  WeakCallCounterAndPersistent<Value> g3s2(&counter);

  {
    HandleScope scope(iso);
    g1s1.handle.Reset(iso, Object::New(iso));
    g1s2.handle.Reset(iso, Object::New(iso));
    g1s1.handle.SetWeak(&g1s1, &WeakPointerCallback);
    g1s2.handle.SetWeak(&g1s2, &WeakPointerCallback);

    g2s1.handle.Reset(iso, Object::New(iso));
    g2s2.handle.Reset(iso, Object::New(iso));
    g2s1.handle.SetWeak(&g2s1, &WeakPointerCallback);
    g2s2.handle.SetWeak(&g2s2, &WeakPointerCallback);

    g3s1.handle.Reset(iso, Object::New(iso));
    g3s2.handle.Reset(iso, Object::New(iso));
    g3s1.handle.SetWeak(&g3s1, &WeakPointerCallback);
    g3s2.handle.SetWeak(&g3s2, &WeakPointerCallback);
  }

  // Make a root.
  WeakCallCounterAndPersistent<Value> root(&counter);
  root.handle.Reset(iso, g1s1.handle);
  root.handle.MarkPartiallyDependent();

  // Connect groups.  We're building the following cycle:
  // G1: { g1s1, g2s1 }, g1s1 implicitly references g2s1, ditto for other
  // groups.
  {
    HandleScope handle_scope(iso);
    g1s1.handle.MarkPartiallyDependent();
    g1s2.handle.MarkPartiallyDependent();
    g2s1.handle.MarkPartiallyDependent();
    g2s2.handle.MarkPartiallyDependent();
    g3s1.handle.MarkPartiallyDependent();
    g3s2.handle.MarkPartiallyDependent();
    iso->SetObjectGroupId(g1s1.handle, UniqueId(1));
    iso->SetObjectGroupId(g1s2.handle, UniqueId(1));
    Local<Object>::New(iso, g1s1.handle.As<Object>())
        ->Set(v8_str("x"), Local<Value>::New(iso, g2s1.handle));
    iso->SetObjectGroupId(g2s1.handle, UniqueId(2));
    iso->SetObjectGroupId(g2s2.handle, UniqueId(2));
    Local<Object>::New(iso, g2s1.handle.As<Object>())
        ->Set(v8_str("x"), Local<Value>::New(iso, g3s1.handle));
    iso->SetObjectGroupId(g3s1.handle, UniqueId(3));
    iso->SetObjectGroupId(g3s2.handle, UniqueId(3));
    Local<Object>::New(iso, g3s1.handle.As<Object>())
        ->Set(v8_str("x"), Local<Value>::New(iso, g1s1.handle));
  }

  v8::internal::Heap* heap =
      reinterpret_cast<v8::internal::Isolate*>(iso)->heap();
  heap->CollectAllGarbage(i::Heap::kNoGCFlags);

  // All objects should be alive.
  CHECK_EQ(0, counter.NumberOfWeakCalls());

  // Weaken the root.
  root.handle.SetWeak(&root, &WeakPointerCallback);
  root.handle.MarkPartiallyDependent();

  // Groups are deleted, rebuild groups.
  {
    HandleScope handle_scope(iso);
    g1s1.handle.MarkPartiallyDependent();
    g1s2.handle.MarkPartiallyDependent();
    g2s1.handle.MarkPartiallyDependent();
    g2s2.handle.MarkPartiallyDependent();
    g3s1.handle.MarkPartiallyDependent();
    g3s2.handle.MarkPartiallyDependent();
    iso->SetObjectGroupId(g1s1.handle, UniqueId(1));
    iso->SetObjectGroupId(g1s2.handle, UniqueId(1));
    Local<Object>::New(iso, g1s1.handle.As<Object>())
        ->Set(v8_str("x"), Local<Value>::New(iso, g2s1.handle));
    iso->SetObjectGroupId(g2s1.handle, UniqueId(2));
    iso->SetObjectGroupId(g2s2.handle, UniqueId(2));
    Local<Object>::New(iso, g2s1.handle.As<Object>())
        ->Set(v8_str("x"), Local<Value>::New(iso, g3s1.handle));
    iso->SetObjectGroupId(g3s1.handle, UniqueId(3));
    iso->SetObjectGroupId(g3s2.handle, UniqueId(3));
    Local<Object>::New(iso, g3s1.handle.As<Object>())
        ->Set(v8_str("x"), Local<Value>::New(iso, g1s1.handle));
  }

  heap->CollectAllGarbage(i::Heap::kNoGCFlags);

  // All objects should be gone. 7 global handles in total.
  CHECK_EQ(7, counter.NumberOfWeakCalls());
}


THREADED_TEST(ScriptException) {
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());
  Local<Script> script = v8_compile("throw 'panama!';");
  v8::TryCatch try_catch;
  Local<Value> result = script->Run();
  CHECK(result.IsEmpty());
  CHECK(try_catch.HasCaught());
  String::Utf8Value exception_value(try_catch.Exception());
  CHECK_EQ(0, strcmp(*exception_value, "panama!"));
}


TEST(TryCatchCustomException) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);
  v8::TryCatch try_catch;
  CompileRun(
      "function CustomError() { this.a = 'b'; }"
      "(function f() { throw new CustomError(); })();");
  CHECK(try_catch.HasCaught());
  CHECK(try_catch.Exception()
            ->ToObject(isolate)
            ->Get(v8_str("a"))
            ->Equals(v8_str("b")));
}


bool message_received;


static void check_message_0(v8::Handle<v8::Message> message,
                            v8::Handle<Value> data) {
  CHECK_EQ(5.76, data->NumberValue());
  CHECK_EQ(6.75, message->GetScriptOrigin().ResourceName()->NumberValue());
  CHECK(!message->IsSharedCrossOrigin());
  message_received = true;
}


THREADED_TEST(MessageHandler0) {
  message_received = false;
  v8::HandleScope scope(CcTest::isolate());
  CHECK(!message_received);
  LocalContext context;
  v8::V8::AddMessageListener(check_message_0, v8_num(5.76));
  v8::Handle<v8::Script> script = CompileWithOrigin("throw 'error'", "6.75");
  script->Run();
  CHECK(message_received);
  // clear out the message listener
  v8::V8::RemoveMessageListeners(check_message_0);
}


static void check_message_1(v8::Handle<v8::Message> message,
                            v8::Handle<Value> data) {
  CHECK(data->IsNumber());
  CHECK_EQ(1337, data->Int32Value());
  CHECK(!message->IsSharedCrossOrigin());
  message_received = true;
}


TEST(MessageHandler1) {
  message_received = false;
  v8::HandleScope scope(CcTest::isolate());
  CHECK(!message_received);
  v8::V8::AddMessageListener(check_message_1);
  LocalContext context;
  CompileRun("throw 1337;");
  CHECK(message_received);
  // clear out the message listener
  v8::V8::RemoveMessageListeners(check_message_1);
}


static void check_message_2(v8::Handle<v8::Message> message,
                            v8::Handle<Value> data) {
  LocalContext context;
  CHECK(data->IsObject());
  v8::Local<v8::Value> hidden_property =
      v8::Object::Cast(*data)->GetHiddenValue(v8_str("hidden key"));
  CHECK(v8_str("hidden value")->Equals(hidden_property));
  CHECK(!message->IsSharedCrossOrigin());
  message_received = true;
}


TEST(MessageHandler2) {
  message_received = false;
  v8::HandleScope scope(CcTest::isolate());
  CHECK(!message_received);
  v8::V8::AddMessageListener(check_message_2);
  LocalContext context;
  v8::Local<v8::Value> error = v8::Exception::Error(v8_str("custom error"));
  v8::Object::Cast(*error)
      ->SetHiddenValue(v8_str("hidden key"), v8_str("hidden value"));
  context->Global()->Set(v8_str("error"), error);
  CompileRun("throw error;");
  CHECK(message_received);
  // clear out the message listener
  v8::V8::RemoveMessageListeners(check_message_2);
}


static void check_message_3(v8::Handle<v8::Message> message,
                            v8::Handle<Value> data) {
  CHECK(message->IsSharedCrossOrigin());
  CHECK(message->GetScriptOrigin().ResourceIsSharedCrossOrigin()->Value());
  CHECK(message->GetScriptOrigin().ResourceIsEmbedderDebugScript()->Value());
  CHECK_EQ(6.75, message->GetScriptOrigin().ResourceName()->NumberValue());
  CHECK_EQ(7.40, message->GetScriptOrigin().SourceMapUrl()->NumberValue());
  message_received = true;
}


TEST(MessageHandler3) {
  message_received = false;
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  CHECK(!message_received);
  v8::V8::AddMessageListener(check_message_3);
  LocalContext context;
  v8::ScriptOrigin origin = v8::ScriptOrigin(
      v8_str("6.75"), v8::Integer::New(isolate, 1),
      v8::Integer::New(isolate, 2), v8::True(isolate), Handle<v8::Integer>(),
      v8::True(isolate), v8_str("7.40"));
  v8::Handle<v8::Script> script =
      Script::Compile(v8_str("throw 'error'"), &origin);
  script->Run();
  CHECK(message_received);
  // clear out the message listener
  v8::V8::RemoveMessageListeners(check_message_3);
}


static void check_message_4(v8::Handle<v8::Message> message,
                            v8::Handle<Value> data) {
  CHECK(!message->IsSharedCrossOrigin());
  CHECK_EQ(6.75, message->GetScriptOrigin().ResourceName()->NumberValue());
  message_received = true;
}


TEST(MessageHandler4) {
  message_received = false;
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  CHECK(!message_received);
  v8::V8::AddMessageListener(check_message_4);
  LocalContext context;
  v8::ScriptOrigin origin =
      v8::ScriptOrigin(v8_str("6.75"), v8::Integer::New(isolate, 1),
                       v8::Integer::New(isolate, 2), v8::False(isolate));
  v8::Handle<v8::Script> script =
      Script::Compile(v8_str("throw 'error'"), &origin);
  script->Run();
  CHECK(message_received);
  // clear out the message listener
  v8::V8::RemoveMessageListeners(check_message_4);
}


static void check_message_5a(v8::Handle<v8::Message> message,
                             v8::Handle<Value> data) {
  CHECK(message->IsSharedCrossOrigin());
  CHECK_EQ(6.75, message->GetScriptOrigin().ResourceName()->NumberValue());
  message_received = true;
}


static void check_message_5b(v8::Handle<v8::Message> message,
                             v8::Handle<Value> data) {
  CHECK(!message->IsSharedCrossOrigin());
  CHECK_EQ(6.75, message->GetScriptOrigin().ResourceName()->NumberValue());
  message_received = true;
}


TEST(MessageHandler5) {
  message_received = false;
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  CHECK(!message_received);
  v8::V8::AddMessageListener(check_message_5a);
  LocalContext context;
  v8::ScriptOrigin origin =
      v8::ScriptOrigin(v8_str("6.75"), v8::Integer::New(isolate, 1),
                       v8::Integer::New(isolate, 2), v8::True(isolate));
  v8::Handle<v8::Script> script =
      Script::Compile(v8_str("throw 'error'"), &origin);
  script->Run();
  CHECK(message_received);
  // clear out the message listener
  v8::V8::RemoveMessageListeners(check_message_5a);

  message_received = false;
  v8::V8::AddMessageListener(check_message_5b);
  origin = v8::ScriptOrigin(v8_str("6.75"), v8::Integer::New(isolate, 1),
                            v8::Integer::New(isolate, 2), v8::False(isolate));
  script = Script::Compile(v8_str("throw 'error'"), &origin);
  script->Run();
  CHECK(message_received);
  // clear out the message listener
  v8::V8::RemoveMessageListeners(check_message_5b);
}


TEST(NativeWeakMap) {
  v8::Isolate* isolate = CcTest::isolate();
  HandleScope scope(isolate);
  Local<v8::NativeWeakMap> weak_map(v8::NativeWeakMap::New(isolate));
  CHECK(!weak_map.IsEmpty());

  LocalContext env;
  Local<Object> value = v8::Object::New(isolate);

  Local<Object> local1 = v8::Object::New(isolate);
  CHECK(!weak_map->Has(local1));
  CHECK(weak_map->Get(local1)->IsUndefined());
  weak_map->Set(local1, value);
  CHECK(weak_map->Has(local1));
  CHECK(value->Equals(weak_map->Get(local1)));

  WeakCallCounter counter(1234);
  WeakCallCounterAndPersistent<Value> o1(&counter);
  WeakCallCounterAndPersistent<Value> o2(&counter);
  WeakCallCounterAndPersistent<Value> s1(&counter);
  {
    HandleScope scope(isolate);
    Local<v8::Object> obj1 = v8::Object::New(isolate);
    Local<v8::Object> obj2 = v8::Object::New(isolate);
    Local<v8::Symbol> sym1 = v8::Symbol::New(isolate);

    weak_map->Set(obj1, value);
    weak_map->Set(obj2, value);
    weak_map->Set(sym1, value);

    o1.handle.Reset(isolate, obj1);
    o2.handle.Reset(isolate, obj2);
    s1.handle.Reset(isolate, sym1);

    CHECK(weak_map->Has(local1));
    CHECK(weak_map->Has(obj1));
    CHECK(weak_map->Has(obj2));
    CHECK(weak_map->Has(sym1));

    CHECK(value->Equals(weak_map->Get(local1)));
    CHECK(value->Equals(weak_map->Get(obj1)));
    CHECK(value->Equals(weak_map->Get(obj2)));
    CHECK(value->Equals(weak_map->Get(sym1)));
  }
  CcTest::heap()->CollectAllGarbage(TestHeap::Heap::kNoGCFlags);
  {
    HandleScope scope(isolate);
    CHECK(value->Equals(weak_map->Get(local1)));
    CHECK(value->Equals(weak_map->Get(Local<Value>::New(isolate, o1.handle))));
    CHECK(value->Equals(weak_map->Get(Local<Value>::New(isolate, o2.handle))));
    CHECK(value->Equals(weak_map->Get(Local<Value>::New(isolate, s1.handle))));
  }

  o1.handle.SetWeak(&o1, &WeakPointerCallback);
  o2.handle.SetWeak(&o2, &WeakPointerCallback);
  s1.handle.SetWeak(&s1, &WeakPointerCallback);

  CcTest::heap()->CollectAllGarbage(TestHeap::Heap::kNoGCFlags);
  CHECK_EQ(3, counter.NumberOfWeakCalls());

  CHECK(o1.handle.IsEmpty());
  CHECK(o2.handle.IsEmpty());
  CHECK(s1.handle.IsEmpty());

  CHECK(value->Equals(weak_map->Get(local1)));
  CHECK(weak_map->Delete(local1));
  CHECK(!weak_map->Has(local1));
  CHECK(weak_map->Get(local1)->IsUndefined());
}


THREADED_TEST(GetSetProperty) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);
  context->Global()->Set(v8_str("foo"), v8_num(14));
  context->Global()->Set(v8_str("12"), v8_num(92));
  context->Global()->Set(v8::Integer::New(isolate, 16), v8_num(32));
  context->Global()->Set(v8_num(13), v8_num(56));
  Local<Value> foo = CompileRun("this.foo");
  CHECK_EQ(14, foo->Int32Value());
  Local<Value> twelve = CompileRun("this[12]");
  CHECK_EQ(92, twelve->Int32Value());
  Local<Value> sixteen = CompileRun("this[16]");
  CHECK_EQ(32, sixteen->Int32Value());
  Local<Value> thirteen = CompileRun("this[13]");
  CHECK_EQ(56, thirteen->Int32Value());
  CHECK_EQ(92,
           context->Global()->Get(v8::Integer::New(isolate, 12))->Int32Value());
  CHECK_EQ(92, context->Global()->Get(v8_str("12"))->Int32Value());
  CHECK_EQ(92, context->Global()->Get(v8_num(12))->Int32Value());
  CHECK_EQ(32,
           context->Global()->Get(v8::Integer::New(isolate, 16))->Int32Value());
  CHECK_EQ(32, context->Global()->Get(v8_str("16"))->Int32Value());
  CHECK_EQ(32, context->Global()->Get(v8_num(16))->Int32Value());
  CHECK_EQ(56,
           context->Global()->Get(v8::Integer::New(isolate, 13))->Int32Value());
  CHECK_EQ(56, context->Global()->Get(v8_str("13"))->Int32Value());
  CHECK_EQ(56, context->Global()->Get(v8_num(13))->Int32Value());
}


THREADED_TEST(PropertyAttributes) {
  LocalContext context;
  v8::HandleScope scope(context->GetIsolate());
  // none
  Local<String> prop = v8_str("none");
  context->Global()->Set(prop, v8_num(7));
  CHECK_EQ(v8::None, context->Global()->GetPropertyAttributes(prop));
  // read-only
  prop = v8_str("read_only");
  context->Global()->ForceSet(prop, v8_num(7), v8::ReadOnly);
  CHECK_EQ(7, context->Global()->Get(prop)->Int32Value());
  CHECK_EQ(v8::ReadOnly, context->Global()->GetPropertyAttributes(prop));
  CompileRun("read_only = 9");
  CHECK_EQ(7, context->Global()->Get(prop)->Int32Value());
  context->Global()->Set(prop, v8_num(10));
  CHECK_EQ(7, context->Global()->Get(prop)->Int32Value());
  // dont-delete
  prop = v8_str("dont_delete");
  context->Global()->ForceSet(prop, v8_num(13), v8::DontDelete);
  CHECK_EQ(13, context->Global()->Get(prop)->Int32Value());
  CompileRun("delete dont_delete");
  CHECK_EQ(13, context->Global()->Get(prop)->Int32Value());
  CHECK_EQ(v8::DontDelete, context->Global()->GetPropertyAttributes(prop));
  // dont-enum
  prop = v8_str("dont_enum");
  context->Global()->ForceSet(prop, v8_num(28), v8::DontEnum);
  CHECK_EQ(v8::DontEnum, context->Global()->GetPropertyAttributes(prop));
  // absent
  prop = v8_str("absent");
  CHECK_EQ(v8::None, context->Global()->GetPropertyAttributes(prop));
  Local<Value> fake_prop = v8_num(1);
  CHECK_EQ(v8::None, context->Global()->GetPropertyAttributes(fake_prop));
  // exception
  TryCatch try_catch;
  Local<Value> exception =
      CompileRun("({ toString: function() { throw 'exception';} })");
  CHECK_EQ(v8::None, context->Global()->GetPropertyAttributes(exception));
  CHECK(try_catch.HasCaught());
  String::Utf8Value exception_value(try_catch.Exception());
  CHECK_EQ(0, strcmp("exception", *exception_value));
  try_catch.Reset();
}


THREADED_TEST(Array) {
  LocalContext context;
  v8::HandleScope scope(context->GetIsolate());
  Local<v8::Array> array = v8::Array::New(context->GetIsolate());
  CHECK_EQ(0u, array->Length());
  CHECK(array->Get(0)->IsUndefined());
  CHECK(!array->Has(0));
  CHECK(array->Get(100)->IsUndefined());
  CHECK(!array->Has(100));
  array->Set(2, v8_num(7));
  CHECK_EQ(3u, array->Length());
  CHECK(!array->Has(0));
  CHECK(!array->Has(1));
  CHECK(array->Has(2));
  CHECK_EQ(7, array->Get(2)->Int32Value());
  Local<Value> obj = CompileRun("[1, 2, 3]");
  Local<v8::Array> arr = obj.As<v8::Array>();
  CHECK_EQ(3u, arr->Length());
  CHECK_EQ(1, arr->Get(0)->Int32Value());
  CHECK_EQ(2, arr->Get(1)->Int32Value());
  CHECK_EQ(3, arr->Get(2)->Int32Value());
  array = v8::Array::New(context->GetIsolate(), 27);
  CHECK_EQ(27u, array->Length());
  array = v8::Array::New(context->GetIsolate(), -27);
  CHECK_EQ(0u, array->Length());
}


void HandleF(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::EscapableHandleScope scope(args.GetIsolate());
  ApiTestFuzzer::Fuzz();
  Local<v8::Array> result = v8::Array::New(args.GetIsolate(), args.Length());
  for (int i = 0; i < args.Length(); i++) result->Set(i, args[i]);
  args.GetReturnValue().Set(scope.Escape(result));
}


THREADED_TEST(Vector) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  Local<ObjectTemplate> global = ObjectTemplate::New(isolate);
  global->Set(v8_str("f"), v8::FunctionTemplate::New(isolate, HandleF));
  LocalContext context(0, global);

  const char* fun = "f()";
  Local<v8::Array> a0 = CompileRun(fun).As<v8::Array>();
  CHECK_EQ(0u, a0->Length());

  const char* fun2 = "f(11)";
  Local<v8::Array> a1 = CompileRun(fun2).As<v8::Array>();
  CHECK_EQ(1u, a1->Length());
  CHECK_EQ(11, a1->Get(0)->Int32Value());

  const char* fun3 = "f(12, 13)";
  Local<v8::Array> a2 = CompileRun(fun3).As<v8::Array>();
  CHECK_EQ(2u, a2->Length());
  CHECK_EQ(12, a2->Get(0)->Int32Value());
  CHECK_EQ(13, a2->Get(1)->Int32Value());

  const char* fun4 = "f(14, 15, 16)";
  Local<v8::Array> a3 = CompileRun(fun4).As<v8::Array>();
  CHECK_EQ(3u, a3->Length());
  CHECK_EQ(14, a3->Get(0)->Int32Value());
  CHECK_EQ(15, a3->Get(1)->Int32Value());
  CHECK_EQ(16, a3->Get(2)->Int32Value());

  const char* fun5 = "f(17, 18, 19, 20)";
  Local<v8::Array> a4 = CompileRun(fun5).As<v8::Array>();
  CHECK_EQ(4u, a4->Length());
  CHECK_EQ(17, a4->Get(0)->Int32Value());
  CHECK_EQ(18, a4->Get(1)->Int32Value());
  CHECK_EQ(19, a4->Get(2)->Int32Value());
  CHECK_EQ(20, a4->Get(3)->Int32Value());
}


THREADED_TEST(FunctionCall) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);
  CompileRun(
      "function Foo() {"
      "  var result = [];"
      "  for (var i = 0; i < arguments.length; i++) {"
      "    result.push(arguments[i]);"
      "  }"
      "  return result;"
      "}"
      "function ReturnThisSloppy() {"
      "  return this;"
      "}"
      "function ReturnThisStrict() {"
      "  'use strict';"
      "  return this;"
      "}");
  Local<Function> Foo =
      Local<Function>::Cast(context->Global()->Get(v8_str("Foo")));
  Local<Function> ReturnThisSloppy =
      Local<Function>::Cast(context->Global()->Get(v8_str("ReturnThisSloppy")));
  Local<Function> ReturnThisStrict =
      Local<Function>::Cast(context->Global()->Get(v8_str("ReturnThisStrict")));

  v8::Handle<Value>* args0 = NULL;
  Local<v8::Array> a0 = Local<v8::Array>::Cast(Foo->Call(Foo, 0, args0));
  CHECK_EQ(0u, a0->Length());

  v8::Handle<Value> args1[] = {v8_num(1.1)};
  Local<v8::Array> a1 = Local<v8::Array>::Cast(Foo->Call(Foo, 1, args1));
  CHECK_EQ(1u, a1->Length());
  CHECK_EQ(1.1, a1->Get(v8::Integer::New(isolate, 0))->NumberValue());

  v8::Handle<Value> args2[] = {v8_num(2.2), v8_num(3.3)};
  Local<v8::Array> a2 = Local<v8::Array>::Cast(Foo->Call(Foo, 2, args2));
  CHECK_EQ(2u, a2->Length());
  CHECK_EQ(2.2, a2->Get(v8::Integer::New(isolate, 0))->NumberValue());
  CHECK_EQ(3.3, a2->Get(v8::Integer::New(isolate, 1))->NumberValue());

  v8::Handle<Value> args3[] = {v8_num(4.4), v8_num(5.5), v8_num(6.6)};
  Local<v8::Array> a3 = Local<v8::Array>::Cast(Foo->Call(Foo, 3, args3));
  CHECK_EQ(3u, a3->Length());
  CHECK_EQ(4.4, a3->Get(v8::Integer::New(isolate, 0))->NumberValue());
  CHECK_EQ(5.5, a3->Get(v8::Integer::New(isolate, 1))->NumberValue());
  CHECK_EQ(6.6, a3->Get(v8::Integer::New(isolate, 2))->NumberValue());

  v8::Handle<Value> args4[] = {v8_num(7.7), v8_num(8.8), v8_num(9.9),
                               v8_num(10.11)};
  Local<v8::Array> a4 = Local<v8::Array>::Cast(Foo->Call(Foo, 4, args4));
  CHECK_EQ(4u, a4->Length());
  CHECK_EQ(7.7, a4->Get(v8::Integer::New(isolate, 0))->NumberValue());
  CHECK_EQ(8.8, a4->Get(v8::Integer::New(isolate, 1))->NumberValue());
  CHECK_EQ(9.9, a4->Get(v8::Integer::New(isolate, 2))->NumberValue());
  CHECK_EQ(10.11, a4->Get(v8::Integer::New(isolate, 3))->NumberValue());

  Local<v8::Value> r1 = ReturnThisSloppy->Call(v8::Undefined(isolate), 0, NULL);
  CHECK(r1->StrictEquals(context->Global()));
  Local<v8::Value> r2 = ReturnThisSloppy->Call(v8::Null(isolate), 0, NULL);
  CHECK(r2->StrictEquals(context->Global()));
  Local<v8::Value> r3 = ReturnThisSloppy->Call(v8_num(42), 0, NULL);
  CHECK(r3->IsNumberObject());
  CHECK_EQ(42.0, r3.As<v8::NumberObject>()->ValueOf());
  Local<v8::Value> r4 = ReturnThisSloppy->Call(v8_str("hello"), 0, NULL);
  CHECK(r4->IsStringObject());
  CHECK(r4.As<v8::StringObject>()->ValueOf()->StrictEquals(v8_str("hello")));
  Local<v8::Value> r5 = ReturnThisSloppy->Call(v8::True(isolate), 0, NULL);
  CHECK(r5->IsBooleanObject());
  CHECK(r5.As<v8::BooleanObject>()->ValueOf());

  Local<v8::Value> r6 = ReturnThisStrict->Call(v8::Undefined(isolate), 0, NULL);
  CHECK(r6->IsUndefined());
  Local<v8::Value> r7 = ReturnThisStrict->Call(v8::Null(isolate), 0, NULL);
  CHECK(r7->IsNull());
  Local<v8::Value> r8 = ReturnThisStrict->Call(v8_num(42), 0, NULL);
  CHECK(r8->StrictEquals(v8_num(42)));
  Local<v8::Value> r9 = ReturnThisStrict->Call(v8_str("hello"), 0, NULL);
  CHECK(r9->StrictEquals(v8_str("hello")));
  Local<v8::Value> r10 = ReturnThisStrict->Call(v8::True(isolate), 0, NULL);
  CHECK(r10->StrictEquals(v8::True(isolate)));
}


THREADED_TEST(ConstructCall) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);
  CompileRun(
      "function Foo() {"
      "  var result = [];"
      "  for (var i = 0; i < arguments.length; i++) {"
      "    result.push(arguments[i]);"
      "  }"
      "  return result;"
      "}");
  Local<Function> Foo =
      Local<Function>::Cast(context->Global()->Get(v8_str("Foo")));

  v8::Handle<Value>* args0 = NULL;
  Local<v8::Array> a0 = Local<v8::Array>::Cast(Foo->NewInstance(0, args0));
  CHECK_EQ(0u, a0->Length());

  v8::Handle<Value> args1[] = {v8_num(1.1)};
  Local<v8::Array> a1 = Local<v8::Array>::Cast(Foo->NewInstance(1, args1));
  CHECK_EQ(1u, a1->Length());
  CHECK_EQ(1.1, a1->Get(v8::Integer::New(isolate, 0))->NumberValue());

  v8::Handle<Value> args2[] = {v8_num(2.2), v8_num(3.3)};
  Local<v8::Array> a2 = Local<v8::Array>::Cast(Foo->NewInstance(2, args2));
  CHECK_EQ(2u, a2->Length());
  CHECK_EQ(2.2, a2->Get(v8::Integer::New(isolate, 0))->NumberValue());
  CHECK_EQ(3.3, a2->Get(v8::Integer::New(isolate, 1))->NumberValue());

  v8::Handle<Value> args3[] = {v8_num(4.4), v8_num(5.5), v8_num(6.6)};
  Local<v8::Array> a3 = Local<v8::Array>::Cast(Foo->NewInstance(3, args3));
  CHECK_EQ(3u, a3->Length());
  CHECK_EQ(4.4, a3->Get(v8::Integer::New(isolate, 0))->NumberValue());
  CHECK_EQ(5.5, a3->Get(v8::Integer::New(isolate, 1))->NumberValue());
  CHECK_EQ(6.6, a3->Get(v8::Integer::New(isolate, 2))->NumberValue());

  v8::Handle<Value> args4[] = {v8_num(7.7), v8_num(8.8), v8_num(9.9),
                               v8_num(10.11)};
  Local<v8::Array> a4 = Local<v8::Array>::Cast(Foo->NewInstance(4, args4));
  CHECK_EQ(4u, a4->Length());
  CHECK_EQ(7.7, a4->Get(v8::Integer::New(isolate, 0))->NumberValue());
  CHECK_EQ(8.8, a4->Get(v8::Integer::New(isolate, 1))->NumberValue());
  CHECK_EQ(9.9, a4->Get(v8::Integer::New(isolate, 2))->NumberValue());
  CHECK_EQ(10.11, a4->Get(v8::Integer::New(isolate, 3))->NumberValue());
}


static void CheckUncle(v8::TryCatch* try_catch) {
  CHECK(try_catch->HasCaught());
  String::Utf8Value str_value(try_catch->Exception());
  CHECK_EQ(0, strcmp(*str_value, "uncle?"));
  try_catch->Reset();
}


THREADED_TEST(ConversionNumber) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);
  // Very large number.
  CompileRun("var obj = Math.pow(2,32) * 1237;");
  Local<Value> obj = env->Global()->Get(v8_str("obj"));
  CHECK_EQ(5312874545152.0, obj->ToNumber(isolate)->Value());
  CHECK_EQ(0, obj->ToInt32(isolate)->Value());
  CHECK(0u ==
        obj->ToUint32(isolate)->Value());  // NOLINT - no CHECK_EQ for unsigned.
  // Large number.
  CompileRun("var obj = -1234567890123;");
  obj = env->Global()->Get(v8_str("obj"));
  CHECK_EQ(-1234567890123.0, obj->ToNumber(isolate)->Value());
  CHECK_EQ(-1912276171, obj->ToInt32(isolate)->Value());
  CHECK(2382691125u == obj->ToUint32(isolate)->Value());  // NOLINT
  // Small positive integer.
  CompileRun("var obj = 42;");
  obj = env->Global()->Get(v8_str("obj"));
  CHECK_EQ(42.0, obj->ToNumber(isolate)->Value());
  CHECK_EQ(42, obj->ToInt32(isolate)->Value());
  CHECK(42u == obj->ToUint32(isolate)->Value());  // NOLINT
  // Negative integer.
  CompileRun("var obj = -37;");
  obj = env->Global()->Get(v8_str("obj"));
  CHECK_EQ(-37.0, obj->ToNumber(isolate)->Value());
  CHECK_EQ(-37, obj->ToInt32(isolate)->Value());
  CHECK(4294967259u == obj->ToUint32(isolate)->Value());  // NOLINT
  // Positive non-int32 integer.
  CompileRun("var obj = 0x81234567;");
  obj = env->Global()->Get(v8_str("obj"));
  CHECK_EQ(2166572391.0, obj->ToNumber(isolate)->Value());
  CHECK_EQ(-2128394905, obj->ToInt32(isolate)->Value());
  CHECK(2166572391u == obj->ToUint32(isolate)->Value());  // NOLINT
  // Fraction.
  CompileRun("var obj = 42.3;");
  obj = env->Global()->Get(v8_str("obj"));
  CHECK_EQ(42.3, obj->ToNumber(isolate)->Value());
  CHECK_EQ(42, obj->ToInt32(isolate)->Value());
  CHECK(42u == obj->ToUint32(isolate)->Value());  // NOLINT
  // Large negative fraction.
  CompileRun("var obj = -5726623061.75;");
  obj = env->Global()->Get(v8_str("obj"));
  CHECK_EQ(-5726623061.75, obj->ToNumber(isolate)->Value());
  CHECK_EQ(-1431655765, obj->ToInt32(isolate)->Value());
  CHECK(2863311531u == obj->ToUint32(isolate)->Value());  // NOLINT
}


THREADED_TEST(isNumberType) {
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());
  // Very large number.
  CompileRun("var obj = Math.pow(2,32) * 1237;");
  Local<Value> obj = env->Global()->Get(v8_str("obj"));
  CHECK(!obj->IsInt32());
  CHECK(!obj->IsUint32());
  // Large negative number.
  CompileRun("var obj = -1234567890123;");
  obj = env->Global()->Get(v8_str("obj"));
  CHECK(!obj->IsInt32());
  CHECK(!obj->IsUint32());
  // Small positive integer.
  CompileRun("var obj = 42;");
  obj = env->Global()->Get(v8_str("obj"));
  CHECK(obj->IsInt32());
  CHECK(obj->IsUint32());
  // Negative integer.
  CompileRun("var obj = -37;");
  obj = env->Global()->Get(v8_str("obj"));
  CHECK(obj->IsInt32());
  CHECK(!obj->IsUint32());
  // Positive non-int32 integer.
  CompileRun("var obj = 0x81234567;");
  obj = env->Global()->Get(v8_str("obj"));
  CHECK(!obj->IsInt32());
  CHECK(obj->IsUint32());
  // Fraction.
  CompileRun("var obj = 42.3;");
  obj = env->Global()->Get(v8_str("obj"));
  CHECK(!obj->IsInt32());
  CHECK(!obj->IsUint32());
  // Large negative fraction.
  CompileRun("var obj = -5726623061.75;");
  obj = env->Global()->Get(v8_str("obj"));
  CHECK(!obj->IsInt32());
  CHECK(!obj->IsUint32());
  // Positive zero
  CompileRun("var obj = 0.0;");
  obj = env->Global()->Get(v8_str("obj"));
  CHECK(obj->IsInt32());
  CHECK(obj->IsUint32());
  // Positive zero
  CompileRun("var obj = -0.0;");
  obj = env->Global()->Get(v8_str("obj"));
  CHECK(!obj->IsInt32());
  CHECK(!obj->IsUint32());
}


THREADED_TEST(ConversionException) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);
  CompileRun(
      "function TestClass() { };"
      "TestClass.prototype.toString = function () { throw 'uncle?'; };"
      "var obj = new TestClass();");
  Local<Value> obj = env->Global()->Get(v8_str("obj"));

  v8::TryCatch try_catch(isolate);

  Local<Value> to_string_result = obj->ToString(isolate);
  CHECK(to_string_result.IsEmpty());
  CheckUncle(&try_catch);

  Local<Value> to_number_result = obj->ToNumber(isolate);
  CHECK(to_number_result.IsEmpty());
  CheckUncle(&try_catch);

  Local<Value> to_integer_result = obj->ToInteger(isolate);
  CHECK(to_integer_result.IsEmpty());
  CheckUncle(&try_catch);

  Local<Value> to_uint32_result = obj->ToUint32(isolate);
  CHECK(to_uint32_result.IsEmpty());
  CheckUncle(&try_catch);

  Local<Value> to_int32_result = obj->ToInt32(isolate);
  CHECK(to_int32_result.IsEmpty());
  CheckUncle(&try_catch);

  Local<Value> to_object_result = v8::Undefined(isolate)->ToObject(isolate);
  CHECK(to_object_result.IsEmpty());
  CHECK(try_catch.HasCaught());
  try_catch.Reset();

  int32_t int32_value = obj->Int32Value();
  CHECK_EQ(0, int32_value);
  CheckUncle(&try_catch);

  uint32_t uint32_value = obj->Uint32Value();
  CHECK_EQ(0u, uint32_value);
  CheckUncle(&try_catch);

  double number_value = obj->NumberValue();
  CHECK(std::isnan(number_value));
  CheckUncle(&try_catch);

  int64_t integer_value = obj->IntegerValue();
  CHECK_EQ(0, integer_value);
  CheckUncle(&try_catch);
}


void ThrowFromC(const v8::FunctionCallbackInfo<v8::Value>& args) {
  ApiTestFuzzer::Fuzz();
  args.GetIsolate()->ThrowException(v8_str("konto"));
}


void CCatcher(const v8::FunctionCallbackInfo<v8::Value>& args) {
  if (args.Length() < 1) {
    args.GetReturnValue().Set(false);
    return;
  }
  v8::HandleScope scope(args.GetIsolate());
  v8::TryCatch try_catch;
  Local<Value> result = CompileRun(args[0]->ToString(args.GetIsolate()));
  CHECK(!try_catch.HasCaught() || result.IsEmpty());
  args.GetReturnValue().Set(try_catch.HasCaught());
}


THREADED_TEST(APICatch) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  Local<ObjectTemplate> templ = ObjectTemplate::New(isolate);
  templ->Set(v8_str("ThrowFromC"),
             v8::FunctionTemplate::New(isolate, ThrowFromC));
  LocalContext context(0, templ);
  CompileRun(
      "var thrown = false;"
      "try {"
      "  ThrowFromC();"
      "} catch (e) {"
      "  thrown = true;"
      "}");
  Local<Value> thrown = context->Global()->Get(v8_str("thrown"));
  CHECK(thrown->BooleanValue());
}


THREADED_TEST(APIThrowTryCatch) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  Local<ObjectTemplate> templ = ObjectTemplate::New(isolate);
  templ->Set(v8_str("ThrowFromC"),
             v8::FunctionTemplate::New(isolate, ThrowFromC));
  LocalContext context(0, templ);
  v8::TryCatch try_catch;
  CompileRun("ThrowFromC();");
  CHECK(try_catch.HasCaught());
}


// Test that a try-finally block doesn't shadow a try-catch block
// when setting up an external handler.
//
// BUG(271): Some of the exception propagation does not work on the
// ARM simulator because the simulator separates the C++ stack and the
// JS stack.  This test therefore fails on the simulator.  The test is
// not threaded to allow the threading tests to run on the simulator.
TEST(TryCatchInTryFinally) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  Local<ObjectTemplate> templ = ObjectTemplate::New(isolate);
  templ->Set(v8_str("CCatcher"), v8::FunctionTemplate::New(isolate, CCatcher));
  LocalContext context(0, templ);
  Local<Value> result = CompileRun(
      "try {"
      "  try {"
      "    CCatcher('throw 7;');"
      "  } finally {"
      "  }"
      "} catch (e) {"
      "}");
  CHECK(result->IsTrue());
}


static void check_reference_error_message(v8::Handle<v8::Message> message,
                                          v8::Handle<v8::Value> data) {
  const char* reference_error = "Uncaught ReferenceError: asdf is not defined";
  CHECK(message->Get()->Equals(v8_str(reference_error)));
}


static void Fail(const v8::FunctionCallbackInfo<v8::Value>& args) {
  ApiTestFuzzer::Fuzz();
  CHECK(false);
}


// Test that overwritten methods are not invoked on uncaught exception
// formatting. However, they are invoked when performing normal error
// string conversions.
TEST(APIThrowMessageOverwrittenToString) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  v8::V8::AddMessageListener(check_reference_error_message);
  Local<ObjectTemplate> templ = ObjectTemplate::New(isolate);
  templ->Set(v8_str("fail"), v8::FunctionTemplate::New(isolate, Fail));
  LocalContext context(NULL, templ);
  CompileRun("asdf;");
  CompileRun(
      "var limit = {};"
      "limit.valueOf = fail;"
      "Error.stackTraceLimit = limit;");
  CompileRun("asdf");
  CompileRun("Array.prototype.pop = fail;");
  CompileRun("Object.prototype.hasOwnProperty = fail;");
  CompileRun("Object.prototype.toString = function f() { return 'Yikes'; }");
  CompileRun("Number.prototype.toString = function f() { return 'Yikes'; }");
  CompileRun("String.prototype.toString = function f() { return 'Yikes'; }");
  CompileRun(
      "ReferenceError.prototype.toString ="
      "  function() { return 'Whoops' }");
  CompileRun("asdf;");
  CompileRun("ReferenceError.prototype.constructor.name = void 0;");
  CompileRun("asdf;");
  CompileRun("ReferenceError.prototype.constructor = void 0;");
  CompileRun("asdf;");
  CompileRun("ReferenceError.prototype.__proto__ = new Object();");
  CompileRun("asdf;");
  CompileRun("ReferenceError.prototype = new Object();");
  CompileRun("asdf;");
  v8::Handle<Value> string = CompileRun("try { asdf; } catch(e) { e + ''; }");
  CHECK(string->Equals(v8_str("Whoops")));
  CompileRun(
      "ReferenceError.prototype.constructor = new Object();"
      "ReferenceError.prototype.constructor.name = 1;"
      "Number.prototype.toString = function() { return 'Whoops'; };"
      "ReferenceError.prototype.toString = Object.prototype.toString;");
  CompileRun("asdf;");
  v8::V8::RemoveMessageListeners(check_reference_error_message);
}


static void check_custom_error_tostring(v8::Handle<v8::Message> message,
                                        v8::Handle<v8::Value> data) {
  const char* uncaught_error = "Uncaught MyError toString";
  CHECK(message->Get()->Equals(v8_str(uncaught_error)));
}


TEST(CustomErrorToString) {
  LocalContext context;
  v8::HandleScope scope(context->GetIsolate());
  v8::V8::AddMessageListener(check_custom_error_tostring);
  CompileRun(
      "function MyError(name, message) {                   "
      "  this.name = name;                                 "
      "  this.message = message;                           "
      "}                                                   "
      "MyError.prototype = Object.create(Error.prototype); "
      "MyError.prototype.toString = function() {           "
      "  return 'MyError toString';                        "
      "};                                                  "
      "throw new MyError('my name', 'my message');         ");
  v8::V8::RemoveMessageListeners(check_custom_error_tostring);
}


static void check_custom_error_message(v8::Handle<v8::Message> message,
                                       v8::Handle<v8::Value> data) {
  const char* uncaught_error = "Uncaught MyError: my message";
  printf("%s\n", *v8::String::Utf8Value(message->Get()));
  CHECK(message->Get()->Equals(v8_str(uncaught_error)));
}


TEST(CustomErrorMessage) {
  LocalContext context;
  v8::HandleScope scope(context->GetIsolate());
  v8::V8::AddMessageListener(check_custom_error_message);

  // Handlebars.
  CompileRun(
      "function MyError(msg) {                             "
      "  this.name = 'MyError';                            "
      "  this.message = msg;                               "
      "}                                                   "
      "MyError.prototype = new Error();                    "
      "throw new MyError('my message');                    ");

  // Closure.
  CompileRun(
      "function MyError(msg) {                             "
      "  this.name = 'MyError';                            "
      "  this.message = msg;                               "
      "}                                                   "
      "inherits = function(childCtor, parentCtor) {        "
      "    function tempCtor() {};                         "
      "    tempCtor.prototype = parentCtor.prototype;      "
      "    childCtor.superClass_ = parentCtor.prototype;   "
      "    childCtor.prototype = new tempCtor();           "
      "    childCtor.prototype.constructor = childCtor;    "
      "};                                                  "
      "inherits(MyError, Error);                           "
      "throw new MyError('my message');                    ");

  // Object.create.
  CompileRun(
      "function MyError(msg) {                             "
      "  this.name = 'MyError';                            "
      "  this.message = msg;                               "
      "}                                                   "
      "MyError.prototype = Object.create(Error.prototype); "
      "throw new MyError('my message');                    ");

  v8::V8::RemoveMessageListeners(check_custom_error_message);
}


static void receive_message(v8::Handle<v8::Message> message,
                            v8::Handle<v8::Value> data) {
  message->Get();
  message_received = true;
}


TEST(APIThrowMessage) {
  message_received = false;
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  v8::V8::AddMessageListener(receive_message);
  Local<ObjectTemplate> templ = ObjectTemplate::New(isolate);
  templ->Set(v8_str("ThrowFromC"),
             v8::FunctionTemplate::New(isolate, ThrowFromC));
  LocalContext context(0, templ);
  CompileRun("ThrowFromC();");
  CHECK(message_received);
  v8::V8::RemoveMessageListeners(receive_message);
}


TEST(APIThrowMessageAndVerboseTryCatch) {
  message_received = false;
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  v8::V8::AddMessageListener(receive_message);
  Local<ObjectTemplate> templ = ObjectTemplate::New(isolate);
  templ->Set(v8_str("ThrowFromC"),
             v8::FunctionTemplate::New(isolate, ThrowFromC));
  LocalContext context(0, templ);
  v8::TryCatch try_catch;
  try_catch.SetVerbose(true);
  Local<Value> result = CompileRun("ThrowFromC();");
  CHECK(try_catch.HasCaught());
  CHECK(result.IsEmpty());
  CHECK(message_received);
  v8::V8::RemoveMessageListeners(receive_message);
}


TEST(APIStackOverflowAndVerboseTryCatch) {
  message_received = false;
  LocalContext context;
  v8::HandleScope scope(context->GetIsolate());
  v8::V8::AddMessageListener(receive_message);
  v8::TryCatch try_catch;
  try_catch.SetVerbose(true);
  Local<Value> result = CompileRun("function foo() { foo(); } foo();");
  CHECK(try_catch.HasCaught());
  CHECK(result.IsEmpty());
  CHECK(message_received);
  v8::V8::RemoveMessageListeners(receive_message);
}


THREADED_TEST(ExternalScriptException) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  Local<ObjectTemplate> templ = ObjectTemplate::New(isolate);
  templ->Set(v8_str("ThrowFromC"),
             v8::FunctionTemplate::New(isolate, ThrowFromC));
  LocalContext context(0, templ);

  v8::TryCatch try_catch;
  Local<Value> result = CompileRun("ThrowFromC(); throw 'panama';");
  CHECK(result.IsEmpty());
  CHECK(try_catch.HasCaught());
  String::Utf8Value exception_value(try_catch.Exception());
  CHECK_EQ(0, strcmp("konto", *exception_value));
}


void CThrowCountDown(const v8::FunctionCallbackInfo<v8::Value>& args) {
  ApiTestFuzzer::Fuzz();
  CHECK_EQ(4, args.Length());
  int count = args[0]->Int32Value();
  int cInterval = args[2]->Int32Value();
  if (count == 0) {
    args.GetIsolate()->ThrowException(v8_str("FromC"));
    return;
  } else {
    Local<v8::Object> global = args.GetIsolate()->GetCurrentContext()->Global();
    Local<Value> fun = global->Get(v8_str("JSThrowCountDown"));
    v8::Handle<Value> argv[] = {v8_num(count - 1), args[1], args[2], args[3]};
    if (count % cInterval == 0) {
      v8::TryCatch try_catch;
      Local<Value> result = fun.As<Function>()->Call(global, 4, argv);
      int expected = args[3]->Int32Value();
      if (try_catch.HasCaught()) {
        CHECK_EQ(expected, count);
        CHECK(result.IsEmpty());
        CHECK(!CcTest::i_isolate()->has_scheduled_exception());
      } else {
        CHECK_NE(expected, count);
      }
      args.GetReturnValue().Set(result);
      return;
    } else {
      args.GetReturnValue().Set(fun.As<Function>()->Call(global, 4, argv));
      return;
    }
  }
}


void JSCheck(const v8::FunctionCallbackInfo<v8::Value>& args) {
  ApiTestFuzzer::Fuzz();
  CHECK_EQ(3, args.Length());
  bool equality = args[0]->BooleanValue();
  int count = args[1]->Int32Value();
  int expected = args[2]->Int32Value();
  if (equality) {
    CHECK_EQ(count, expected);
  } else {
    CHECK_NE(count, expected);
  }
}


THREADED_TEST(EvalInTryFinally) {
  LocalContext context;
  v8::HandleScope scope(context->GetIsolate());
  v8::TryCatch try_catch;
  CompileRun(
      "(function() {"
      "  try {"
      "    eval('asldkf (*&^&*^');"
      "  } finally {"
      "    return;"
      "  }"
      "})()");
  CHECK(!try_catch.HasCaught());
}


// This test works by making a stack of alternating JavaScript and C
// activations.  These activations set up exception handlers with regular
// intervals, one interval for C activations and another for JavaScript
// activations.  When enough activations have been created an exception is
// thrown and we check that the right activation catches the exception and that
// no other activations do.  The right activation is always the topmost one with
// a handler, regardless of whether it is in JavaScript or C.
//
// The notation used to describe a test case looks like this:
//
//    *JS[4] *C[3] @JS[2] C[1] JS[0]
//
// Each entry is an activation, either JS or C.  The index is the count at that
// level.  Stars identify activations with exception handlers, the @ identifies
// the exception handler that should catch the exception.
//
// BUG(271): Some of the exception propagation does not work on the
// ARM simulator because the simulator separates the C++ stack and the
// JS stack.  This test therefore fails on the simulator.  The test is
// not threaded to allow the threading tests to run on the simulator.
TEST(ExceptionOrder) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  Local<ObjectTemplate> templ = ObjectTemplate::New(isolate);
  templ->Set(v8_str("check"), v8::FunctionTemplate::New(isolate, JSCheck));
  templ->Set(v8_str("CThrowCountDown"),
             v8::FunctionTemplate::New(isolate, CThrowCountDown));
  LocalContext context(0, templ);
  CompileRun(
      "function JSThrowCountDown(count, jsInterval, cInterval, expected) {"
      "  if (count == 0) throw 'FromJS';"
      "  if (count % jsInterval == 0) {"
      "    try {"
      "      var value = CThrowCountDown(count - 1,"
      "                                  jsInterval,"
      "                                  cInterval,"
      "                                  expected);"
      "      check(false, count, expected);"
      "      return value;"
      "    } catch (e) {"
      "      check(true, count, expected);"
      "    }"
      "  } else {"
      "    return CThrowCountDown(count - 1, jsInterval, cInterval, expected);"
      "  }"
      "}");
  Local<Function> fun =
      Local<Function>::Cast(context->Global()->Get(v8_str("JSThrowCountDown")));

  const int argc = 4;
  //                             count      jsInterval cInterval  expected

  // *JS[4] *C[3] @JS[2] C[1] JS[0]
  v8::Handle<Value> a0[argc] = {v8_num(4), v8_num(2), v8_num(3), v8_num(2)};
  fun->Call(fun, argc, a0);

  // JS[5] *C[4] JS[3] @C[2] JS[1] C[0]
  v8::Handle<Value> a1[argc] = {v8_num(5), v8_num(6), v8_num(1), v8_num(2)};
  fun->Call(fun, argc, a1);

  // JS[6] @C[5] JS[4] C[3] JS[2] C[1] JS[0]
  v8::Handle<Value> a2[argc] = {v8_num(6), v8_num(7), v8_num(5), v8_num(5)};
  fun->Call(fun, argc, a2);

  // @JS[6] C[5] JS[4] C[3] JS[2] C[1] JS[0]
  v8::Handle<Value> a3[argc] = {v8_num(6), v8_num(6), v8_num(7), v8_num(6)};
  fun->Call(fun, argc, a3);

  // JS[6] *C[5] @JS[4] C[3] JS[2] C[1] JS[0]
  v8::Handle<Value> a4[argc] = {v8_num(6), v8_num(4), v8_num(5), v8_num(4)};
  fun->Call(fun, argc, a4);

  // JS[6] C[5] *JS[4] @C[3] JS[2] C[1] JS[0]
  v8::Handle<Value> a5[argc] = {v8_num(6), v8_num(4), v8_num(3), v8_num(3)};
  fun->Call(fun, argc, a5);
}


void ThrowValue(const v8::FunctionCallbackInfo<v8::Value>& args) {
  ApiTestFuzzer::Fuzz();
  CHECK_EQ(1, args.Length());
  args.GetIsolate()->ThrowException(args[0]);
}


THREADED_TEST(ThrowValues) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  Local<ObjectTemplate> templ = ObjectTemplate::New(isolate);
  templ->Set(v8_str("Throw"), v8::FunctionTemplate::New(isolate, ThrowValue));
  LocalContext context(0, templ);
  v8::Handle<v8::Array> result = v8::Handle<v8::Array>::Cast(CompileRun(
      "function Run(obj) {"
      "  try {"
      "    Throw(obj);"
      "  } catch (e) {"
      "    return e;"
      "  }"
      "  return 'no exception';"
      "}"
      "[Run('str'), Run(1), Run(0), Run(null), Run(void 0)];"));
  CHECK_EQ(5u, result->Length());
  CHECK(result->Get(v8::Integer::New(isolate, 0))->IsString());
  CHECK(result->Get(v8::Integer::New(isolate, 1))->IsNumber());
  CHECK_EQ(1, result->Get(v8::Integer::New(isolate, 1))->Int32Value());
  CHECK(result->Get(v8::Integer::New(isolate, 2))->IsNumber());
  CHECK_EQ(0, result->Get(v8::Integer::New(isolate, 2))->Int32Value());
  CHECK(result->Get(v8::Integer::New(isolate, 3))->IsNull());
  CHECK(result->Get(v8::Integer::New(isolate, 4))->IsUndefined());
}


THREADED_TEST(CatchZero) {
  LocalContext context;
  v8::HandleScope scope(context->GetIsolate());
  v8::TryCatch try_catch;
  CHECK(!try_catch.HasCaught());
  CompileRun("throw 10");
  CHECK(try_catch.HasCaught());
  CHECK_EQ(10, try_catch.Exception()->Int32Value());
  try_catch.Reset();
  CHECK(!try_catch.HasCaught());
  CompileRun("throw 0");
  CHECK(try_catch.HasCaught());
  CHECK_EQ(0, try_catch.Exception()->Int32Value());
}


THREADED_TEST(CatchExceptionFromWith) {
  LocalContext context;
  v8::HandleScope scope(context->GetIsolate());
  v8::TryCatch try_catch;
  CHECK(!try_catch.HasCaught());
  CompileRun("var o = {}; with (o) { throw 42; }");
  CHECK(try_catch.HasCaught());
}


THREADED_TEST(TryCatchAndFinallyHidingException) {
  LocalContext context;
  v8::HandleScope scope(context->GetIsolate());
  v8::TryCatch try_catch;
  CHECK(!try_catch.HasCaught());
  CompileRun("function f(k) { try { this[k]; } finally { return 0; } };");
  CompileRun("f({toString: function() { throw 42; }});");
  CHECK(!try_catch.HasCaught());
}


void WithTryCatch(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::TryCatch try_catch;
}


THREADED_TEST(TryCatchAndFinally) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);
  context->Global()->Set(
      v8_str("native_with_try_catch"),
      v8::FunctionTemplate::New(isolate, WithTryCatch)->GetFunction());
  v8::TryCatch try_catch;
  CHECK(!try_catch.HasCaught());
  CompileRun(
      "try {\n"
      "  throw new Error('a');\n"
      "} finally {\n"
      "  native_with_try_catch();\n"
      "}\n");
  CHECK(try_catch.HasCaught());
}


static void TryCatchNested1Helper(int depth) {
  if (depth > 0) {
    v8::TryCatch try_catch;
    try_catch.SetVerbose(true);
    TryCatchNested1Helper(depth - 1);
    CHECK(try_catch.HasCaught());
    try_catch.ReThrow();
  } else {
    CcTest::isolate()->ThrowException(v8_str("E1"));
  }
}


static void TryCatchNested2Helper(int depth) {
  if (depth > 0) {
    v8::TryCatch try_catch;
    try_catch.SetVerbose(true);
    TryCatchNested2Helper(depth - 1);
    CHECK(try_catch.HasCaught());
    try_catch.ReThrow();
  } else {
    CompileRun("throw 'E2';");
  }
}


TEST(TryCatchNested) {
  v8::V8::Initialize();
  LocalContext context;
  v8::HandleScope scope(context->GetIsolate());

  {
    // Test nested try-catch with a native throw in the end.
    v8::TryCatch try_catch;
    TryCatchNested1Helper(5);
    CHECK(try_catch.HasCaught());
    CHECK_EQ(0, strcmp(*v8::String::Utf8Value(try_catch.Exception()), "E1"));
  }

  {
    // Test nested try-catch with a JavaScript throw in the end.
    v8::TryCatch try_catch;
    TryCatchNested2Helper(5);
    CHECK(try_catch.HasCaught());
    CHECK_EQ(0, strcmp(*v8::String::Utf8Value(try_catch.Exception()), "E2"));
  }
}


void TryCatchMixedNestingCheck(v8::TryCatch* try_catch) {
  CHECK(try_catch->HasCaught());
  Handle<Message> message = try_catch->Message();
  Handle<Value> resource = message->GetScriptOrigin().ResourceName();
  CHECK_EQ(0, strcmp(*v8::String::Utf8Value(resource), "inner"));
  CHECK_EQ(0,
           strcmp(*v8::String::Utf8Value(message->Get()), "Uncaught Error: a"));
  CHECK_EQ(1, message->GetLineNumber());
  CHECK_EQ(6, message->GetStartColumn());
}


void TryCatchMixedNestingHelper(
    const v8::FunctionCallbackInfo<v8::Value>& args) {
  ApiTestFuzzer::Fuzz();
  v8::TryCatch try_catch;
  CompileRunWithOrigin("throw new Error('a');\n", "inner", 0, 0);
  CHECK(try_catch.HasCaught());
  TryCatchMixedNestingCheck(&try_catch);
  try_catch.ReThrow();
}


// This test ensures that an outer TryCatch in the following situation:
//   C++/TryCatch -> JS -> C++/TryCatch -> JS w/ SyntaxError
// does not clobber the Message object generated for the inner TryCatch.
// This exercises the ability of TryCatch.ReThrow() to restore the
// inner pending Message before throwing the exception again.
TEST(TryCatchMixedNesting) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  v8::V8::Initialize();
  v8::TryCatch try_catch;
  Local<ObjectTemplate> templ = ObjectTemplate::New(isolate);
  templ->Set(v8_str("TryCatchMixedNestingHelper"),
             v8::FunctionTemplate::New(isolate, TryCatchMixedNestingHelper));
  LocalContext context(0, templ);
  CompileRunWithOrigin("TryCatchMixedNestingHelper();\n", "outer", 1, 1);
  TryCatchMixedNestingCheck(&try_catch);
}


void TryCatchNativeHelper(const v8::FunctionCallbackInfo<v8::Value>& args) {
  ApiTestFuzzer::Fuzz();
  v8::TryCatch try_catch;
  args.GetIsolate()->ThrowException(v8_str("boom"));
  CHECK(try_catch.HasCaught());
}


TEST(TryCatchNative) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  v8::V8::Initialize();
  v8::TryCatch try_catch;
  Local<ObjectTemplate> templ = ObjectTemplate::New(isolate);
  templ->Set(v8_str("TryCatchNativeHelper"),
             v8::FunctionTemplate::New(isolate, TryCatchNativeHelper));
  LocalContext context(0, templ);
  CompileRun("TryCatchNativeHelper();");
  CHECK(!try_catch.HasCaught());
}


void TryCatchNativeResetHelper(
    const v8::FunctionCallbackInfo<v8::Value>& args) {
  ApiTestFuzzer::Fuzz();
  v8::TryCatch try_catch;
  args.GetIsolate()->ThrowException(v8_str("boom"));
  CHECK(try_catch.HasCaught());
  try_catch.Reset();
  CHECK(!try_catch.HasCaught());
}


TEST(TryCatchNativeReset) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  v8::V8::Initialize();
  v8::TryCatch try_catch;
  Local<ObjectTemplate> templ = ObjectTemplate::New(isolate);
  templ->Set(v8_str("TryCatchNativeResetHelper"),
             v8::FunctionTemplate::New(isolate, TryCatchNativeResetHelper));
  LocalContext context(0, templ);
  CompileRun("TryCatchNativeResetHelper();");
  CHECK(!try_catch.HasCaught());
}


THREADED_TEST(Equality) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope(context->GetIsolate());
  // Check that equality works at all before relying on CHECK_EQ
  CHECK(v8_str("a")->Equals(v8_str("a")));
  CHECK(!v8_str("a")->Equals(v8_str("b")));

  CHECK(v8_str("a")->Equals(v8_str("a")));
  CHECK(!v8_str("a")->Equals(v8_str("b")));
  CHECK(v8_num(1)->Equals(v8_num(1)));
  CHECK(v8_num(1.00)->Equals(v8_num(1)));
  CHECK(!v8_num(1)->Equals(v8_num(2)));

  // Assume String is not internalized.
  CHECK(v8_str("a")->StrictEquals(v8_str("a")));
  CHECK(!v8_str("a")->StrictEquals(v8_str("b")));
  CHECK(!v8_str("5")->StrictEquals(v8_num(5)));
  CHECK(v8_num(1)->StrictEquals(v8_num(1)));
  CHECK(!v8_num(1)->StrictEquals(v8_num(2)));
  CHECK(v8_num(0.0)->StrictEquals(v8_num(-0.0)));
  Local<Value> not_a_number = v8_num(std::numeric_limits<double>::quiet_NaN());
  CHECK(!not_a_number->StrictEquals(not_a_number));
  CHECK(v8::False(isolate)->StrictEquals(v8::False(isolate)));
  CHECK(!v8::False(isolate)->StrictEquals(v8::Undefined(isolate)));

  v8::Handle<v8::Object> obj = v8::Object::New(isolate);
  v8::Persistent<v8::Object> alias(isolate, obj);
  CHECK(v8::Local<v8::Object>::New(isolate, alias)->StrictEquals(obj));
  alias.Reset();

  CHECK(v8_str("a")->SameValue(v8_str("a")));
  CHECK(!v8_str("a")->SameValue(v8_str("b")));
  CHECK(!v8_str("5")->SameValue(v8_num(5)));
  CHECK(v8_num(1)->SameValue(v8_num(1)));
  CHECK(!v8_num(1)->SameValue(v8_num(2)));
  CHECK(!v8_num(0.0)->SameValue(v8_num(-0.0)));
  CHECK(not_a_number->SameValue(not_a_number));
  CHECK(v8::False(isolate)->SameValue(v8::False(isolate)));
  CHECK(!v8::False(isolate)->SameValue(v8::Undefined(isolate)));
}


THREADED_TEST(MultiRun) {
  LocalContext context;
  v8::HandleScope scope(context->GetIsolate());
  Local<Script> script = v8_compile("x");
  for (int i = 0; i < 10; i++) script->Run();
}


static void GetXValue(Local<String> name,
                      const v8::PropertyCallbackInfo<v8::Value>& info) {
  ApiTestFuzzer::Fuzz();
  CHECK(info.Data()->Equals(v8_str("donut")));
  CHECK(name->Equals(v8_str("x")));
  info.GetReturnValue().Set(name);
}


THREADED_TEST(SimplePropertyRead) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);
  Local<ObjectTemplate> templ = ObjectTemplate::New(isolate);
  templ->SetAccessor(v8_str("x"), GetXValue, NULL, v8_str("donut"));
  context->Global()->Set(v8_str("obj"), templ->NewInstance());
  Local<Script> script = v8_compile("obj.x");
  for (int i = 0; i < 10; i++) {
    Local<Value> result = script->Run();
    CHECK(result->Equals(v8_str("x")));
  }
}


THREADED_TEST(DefinePropertyOnAPIAccessor) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);
  Local<ObjectTemplate> templ = ObjectTemplate::New(isolate);
  templ->SetAccessor(v8_str("x"), GetXValue, NULL, v8_str("donut"));
  context->Global()->Set(v8_str("obj"), templ->NewInstance());

  // Uses getOwnPropertyDescriptor to check the configurable status
  Local<Script> script_desc = v8_compile(
      "var prop = Object.getOwnPropertyDescriptor( "
      "obj, 'x');"
      "prop.configurable;");
  Local<Value> result = script_desc->Run();
  CHECK_EQ(result->BooleanValue(), true);

  // Redefine get - but still configurable
  Local<Script> script_define = v8_compile(
      "var desc = { get: function(){return 42; },"
      "            configurable: true };"
      "Object.defineProperty(obj, 'x', desc);"
      "obj.x");
  result = script_define->Run();
  CHECK(result->Equals(v8_num(42)));

  // Check that the accessor is still configurable
  result = script_desc->Run();
  CHECK_EQ(result->BooleanValue(), true);

  // Redefine to a non-configurable
  script_define = v8_compile(
      "var desc = { get: function(){return 43; },"
      "             configurable: false };"
      "Object.defineProperty(obj, 'x', desc);"
      "obj.x");
  result = script_define->Run();
  CHECK(result->Equals(v8_num(43)));
  result = script_desc->Run();
  CHECK_EQ(result->BooleanValue(), false);

  // Make sure that it is not possible to redefine again
  v8::TryCatch try_catch;
  result = script_define->Run();
  CHECK(try_catch.HasCaught());
  String::Utf8Value exception_value(try_catch.Exception());
  CHECK_EQ(0,
           strcmp(*exception_value, "TypeError: Cannot redefine property: x"));
}


THREADED_TEST(DefinePropertyOnDefineGetterSetter) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  Local<ObjectTemplate> templ = ObjectTemplate::New(isolate);
  templ->SetAccessor(v8_str("x"), GetXValue, NULL, v8_str("donut"));
  LocalContext context;
  context->Global()->Set(v8_str("obj"), templ->NewInstance());

  Local<Script> script_desc = v8_compile(
      "var prop ="
      "Object.getOwnPropertyDescriptor( "
      "obj, 'x');"
      "prop.configurable;");
  Local<Value> result = script_desc->Run();
  CHECK_EQ(result->BooleanValue(), true);

  Local<Script> script_define = v8_compile(
      "var desc = {get: function(){return 42; },"
      "            configurable: true };"
      "Object.defineProperty(obj, 'x', desc);"
      "obj.x");
  result = script_define->Run();
  CHECK(result->Equals(v8_num(42)));


  result = script_desc->Run();
  CHECK_EQ(result->BooleanValue(), true);


  script_define = v8_compile(
      "var desc = {get: function(){return 43; },"
      "            configurable: false };"
      "Object.defineProperty(obj, 'x', desc);"
      "obj.x");
  result = script_define->Run();
  CHECK(result->Equals(v8_num(43)));
  result = script_desc->Run();

  CHECK_EQ(result->BooleanValue(), false);

  v8::TryCatch try_catch;
  result = script_define->Run();
  CHECK(try_catch.HasCaught());
  String::Utf8Value exception_value(try_catch.Exception());
  CHECK_EQ(0,
           strcmp(*exception_value, "TypeError: Cannot redefine property: x"));
}


static v8::Handle<v8::Object> GetGlobalProperty(LocalContext* context,
                                                char const* name) {
  return v8::Handle<v8::Object>::Cast((*context)->Global()->Get(v8_str(name)));
}


THREADED_TEST(DefineAPIAccessorOnObject) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  Local<ObjectTemplate> templ = ObjectTemplate::New(isolate);
  LocalContext context;

  context->Global()->Set(v8_str("obj1"), templ->NewInstance());
  CompileRun("var obj2 = {};");

  CHECK(CompileRun("obj1.x")->IsUndefined());
  CHECK(CompileRun("obj2.x")->IsUndefined());

  CHECK(GetGlobalProperty(&context, "obj1")
            ->SetAccessor(v8_str("x"), GetXValue, NULL, v8_str("donut")));

  ExpectString("obj1.x", "x");
  CHECK(CompileRun("obj2.x")->IsUndefined());

  CHECK(GetGlobalProperty(&context, "obj2")
            ->SetAccessor(v8_str("x"), GetXValue, NULL, v8_str("donut")));

  ExpectString("obj1.x", "x");
  ExpectString("obj2.x", "x");

  ExpectTrue("Object.getOwnPropertyDescriptor(obj1, 'x').configurable");
  ExpectTrue("Object.getOwnPropertyDescriptor(obj2, 'x').configurable");

  CompileRun(
      "Object.defineProperty(obj1, 'x',"
      "{ get: function() { return 'y'; }, configurable: true })");

  ExpectString("obj1.x", "y");
  ExpectString("obj2.x", "x");

  CompileRun(
      "Object.defineProperty(obj2, 'x',"
      "{ get: function() { return 'y'; }, configurable: true })");

  ExpectString("obj1.x", "y");
  ExpectString("obj2.x", "y");

  ExpectTrue("Object.getOwnPropertyDescriptor(obj1, 'x').configurable");
  ExpectTrue("Object.getOwnPropertyDescriptor(obj2, 'x').configurable");

  CHECK(GetGlobalProperty(&context, "obj1")
            ->SetAccessor(v8_str("x"), GetXValue, NULL, v8_str("donut")));
  CHECK(GetGlobalProperty(&context, "obj2")
            ->SetAccessor(v8_str("x"), GetXValue, NULL, v8_str("donut")));

  ExpectString("obj1.x", "x");
  ExpectString("obj2.x", "x");

  ExpectTrue("Object.getOwnPropertyDescriptor(obj1, 'x').configurable");
  ExpectTrue("Object.getOwnPropertyDescriptor(obj2, 'x').configurable");

  // Define getters/setters, but now make them not configurable.
  CompileRun(
      "Object.defineProperty(obj1, 'x',"
      "{ get: function() { return 'z'; }, configurable: false })");
  CompileRun(
      "Object.defineProperty(obj2, 'x',"
      "{ get: function() { return 'z'; }, configurable: false })");

  ExpectTrue("!Object.getOwnPropertyDescriptor(obj1, 'x').configurable");
  ExpectTrue("!Object.getOwnPropertyDescriptor(obj2, 'x').configurable");

  ExpectString("obj1.x", "z");
  ExpectString("obj2.x", "z");

  CHECK(!GetGlobalProperty(&context, "obj1")
             ->SetAccessor(v8_str("x"), GetXValue, NULL, v8_str("donut")));
  CHECK(!GetGlobalProperty(&context, "obj2")
             ->SetAccessor(v8_str("x"), GetXValue, NULL, v8_str("donut")));

  ExpectString("obj1.x", "z");
  ExpectString("obj2.x", "z");
}


THREADED_TEST(DontDeleteAPIAccessorsCannotBeOverriden) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  Local<ObjectTemplate> templ = ObjectTemplate::New(isolate);
  LocalContext context;

  context->Global()->Set(v8_str("obj1"), templ->NewInstance());
  CompileRun("var obj2 = {};");

  CHECK(GetGlobalProperty(&context, "obj1")
            ->SetAccessor(v8_str("x"), GetXValue, NULL, v8_str("donut"),
                          v8::DEFAULT, v8::DontDelete));
  CHECK(GetGlobalProperty(&context, "obj2")
            ->SetAccessor(v8_str("x"), GetXValue, NULL, v8_str("donut"),
                          v8::DEFAULT, v8::DontDelete));

  ExpectString("obj1.x", "x");
  ExpectString("obj2.x", "x");

  ExpectTrue("!Object.getOwnPropertyDescriptor(obj1, 'x').configurable");
  ExpectTrue("!Object.getOwnPropertyDescriptor(obj2, 'x').configurable");

  CHECK(!GetGlobalProperty(&context, "obj1")
             ->SetAccessor(v8_str("x"), GetXValue, NULL, v8_str("donut")));
  CHECK(!GetGlobalProperty(&context, "obj2")
             ->SetAccessor(v8_str("x"), GetXValue, NULL, v8_str("donut")));

  {
    v8::TryCatch try_catch;
    CompileRun(
        "Object.defineProperty(obj1, 'x',"
        "{get: function() { return 'func'; }})");
    CHECK(try_catch.HasCaught());
    String::Utf8Value exception_value(try_catch.Exception());
    CHECK_EQ(
        0, strcmp(*exception_value, "TypeError: Cannot redefine property: x"));
  }
  {
    v8::TryCatch try_catch;
    CompileRun(
        "Object.defineProperty(obj2, 'x',"
        "{get: function() { return 'func'; }})");
    CHECK(try_catch.HasCaught());
    String::Utf8Value exception_value(try_catch.Exception());
    CHECK_EQ(
        0, strcmp(*exception_value, "TypeError: Cannot redefine property: x"));
  }
}


static void Get239Value(Local<String> name,
                        const v8::PropertyCallbackInfo<v8::Value>& info) {
  ApiTestFuzzer::Fuzz();
  CHECK(info.Data()->Equals(v8_str("donut")));
  CHECK(name->Equals(v8_str("239")));
  info.GetReturnValue().Set(name);
}


THREADED_TEST(ElementAPIAccessor) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  Local<ObjectTemplate> templ = ObjectTemplate::New(isolate);
  LocalContext context;

  context->Global()->Set(v8_str("obj1"), templ->NewInstance());
  CompileRun("var obj2 = {};");

  CHECK(GetGlobalProperty(&context, "obj1")
            ->SetAccessor(v8_str("239"), Get239Value, NULL, v8_str("donut")));
  CHECK(GetGlobalProperty(&context, "obj2")
            ->SetAccessor(v8_str("239"), Get239Value, NULL, v8_str("donut")));

  ExpectString("obj1[239]", "239");
  ExpectString("obj2[239]", "239");
  ExpectString("obj1['239']", "239");
  ExpectString("obj2['239']", "239");
}


v8::Persistent<Value> xValue;


static void SetXValue(Local<String> name, Local<Value> value,
                      const v8::PropertyCallbackInfo<void>& info) {
  CHECK(value->Equals(v8_num(4)));
  CHECK(info.Data()->Equals(v8_str("donut")));
  CHECK(name->Equals(v8_str("x")));
  CHECK(xValue.IsEmpty());
  xValue.Reset(info.GetIsolate(), value);
}


THREADED_TEST(SimplePropertyWrite) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  Local<ObjectTemplate> templ = ObjectTemplate::New(isolate);
  templ->SetAccessor(v8_str("x"), GetXValue, SetXValue, v8_str("donut"));
  LocalContext context;
  context->Global()->Set(v8_str("obj"), templ->NewInstance());
  Local<Script> script = v8_compile("obj.x = 4");
  for (int i = 0; i < 10; i++) {
    CHECK(xValue.IsEmpty());
    script->Run();
    CHECK(v8_num(4)->Equals(Local<Value>::New(CcTest::isolate(), xValue)));
    xValue.Reset();
  }
}


THREADED_TEST(SetterOnly) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  Local<ObjectTemplate> templ = ObjectTemplate::New(isolate);
  templ->SetAccessor(v8_str("x"), NULL, SetXValue, v8_str("donut"));
  LocalContext context;
  context->Global()->Set(v8_str("obj"), templ->NewInstance());
  Local<Script> script = v8_compile("obj.x = 4; obj.x");
  for (int i = 0; i < 10; i++) {
    CHECK(xValue.IsEmpty());
    script->Run();
    CHECK(v8_num(4)->Equals(Local<Value>::New(CcTest::isolate(), xValue)));
    xValue.Reset();
  }
}


THREADED_TEST(NoAccessors) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  Local<ObjectTemplate> templ = ObjectTemplate::New(isolate);
  templ->SetAccessor(v8_str("x"), static_cast<v8::AccessorGetterCallback>(NULL),
                     NULL, v8_str("donut"));
  LocalContext context;
  context->Global()->Set(v8_str("obj"), templ->NewInstance());
  Local<Script> script = v8_compile("obj.x = 4; obj.x");
  for (int i = 0; i < 10; i++) {
    script->Run();
  }
}


THREADED_TEST(MultiContexts) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  v8::Handle<ObjectTemplate> templ = ObjectTemplate::New(isolate);
  templ->Set(v8_str("dummy"),
             v8::FunctionTemplate::New(isolate, DummyCallHandler));

  Local<String> password = v8_str("Password");

  // Create an environment
  LocalContext context0(0, templ);
  context0->SetSecurityToken(password);
  v8::Handle<v8::Object> global0 = context0->Global();
  global0->Set(v8_str("custom"), v8_num(1234));
  CHECK_EQ(1234, global0->Get(v8_str("custom"))->Int32Value());

  // Create an independent environment
  LocalContext context1(0, templ);
  context1->SetSecurityToken(password);
  v8::Handle<v8::Object> global1 = context1->Global();
  global1->Set(v8_str("custom"), v8_num(1234));
  CHECK(!global0->Equals(global1));
  CHECK_EQ(1234, global0->Get(v8_str("custom"))->Int32Value());
  CHECK_EQ(1234, global1->Get(v8_str("custom"))->Int32Value());

  // Now create a new context with the old global
  LocalContext context2(0, templ, global1);
  context2->SetSecurityToken(password);
  v8::Handle<v8::Object> global2 = context2->Global();
  CHECK(global1->Equals(global2));
  CHECK_EQ(0, global1->Get(v8_str("custom"))->Int32Value());
  CHECK_EQ(0, global2->Get(v8_str("custom"))->Int32Value());
}


THREADED_TEST(FunctionPrototypeAcrossContexts) {
  // Make sure that functions created by cloning boilerplates cannot
  // communicate through their __proto__ field.

  v8::HandleScope scope(CcTest::isolate());

  LocalContext env0;
  v8::Handle<v8::Object> global0 = env0->Global();
  v8::Handle<v8::Object> object0 =
      global0->Get(v8_str("Object")).As<v8::Object>();
  v8::Handle<v8::Object> tostring0 =
      object0->Get(v8_str("toString")).As<v8::Object>();
  v8::Handle<v8::Object> proto0 =
      tostring0->Get(v8_str("__proto__")).As<v8::Object>();
  proto0->Set(v8_str("custom"), v8_num(1234));

  LocalContext env1;
  v8::Handle<v8::Object> global1 = env1->Global();
  v8::Handle<v8::Object> object1 =
      global1->Get(v8_str("Object")).As<v8::Object>();
  v8::Handle<v8::Object> tostring1 =
      object1->Get(v8_str("toString")).As<v8::Object>();
  v8::Handle<v8::Object> proto1 =
      tostring1->Get(v8_str("__proto__")).As<v8::Object>();
  CHECK(!proto1->Has(v8_str("custom")));
}


THREADED_TEST(Regress892105) {
  // Make sure that object and array literals created by cloning
  // boilerplates cannot communicate through their __proto__
  // field. This is rather difficult to check, but we try to add stuff
  // to Object.prototype and Array.prototype and create a new
  // environment. This should succeed.

  v8::HandleScope scope(CcTest::isolate());

  Local<String> source = v8_str(
      "Object.prototype.obj = 1234;"
      "Array.prototype.arr = 4567;"
      "8901");

  LocalContext env0;
  Local<Script> script0 = v8_compile(source);
  CHECK_EQ(8901.0, script0->Run()->NumberValue());

  LocalContext env1;
  Local<Script> script1 = v8_compile(source);
  CHECK_EQ(8901.0, script1->Run()->NumberValue());
}


THREADED_TEST(UndetectableObject) {
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());

  Local<v8::FunctionTemplate> desc =
      v8::FunctionTemplate::New(env->GetIsolate());
  desc->InstanceTemplate()->MarkAsUndetectable();  // undetectable

  Local<v8::Object> obj = desc->GetFunction()->NewInstance();
  env->Global()->Set(v8_str("undetectable"), obj);

  ExpectString("undetectable.toString()", "[object Object]");
  ExpectString("typeof undetectable", "undefined");
  ExpectString("typeof(undetectable)", "undefined");
  ExpectBoolean("typeof undetectable == 'undefined'", true);
  ExpectBoolean("typeof undetectable == 'object'", false);
  ExpectBoolean("if (undetectable) { true; } else { false; }", false);
  ExpectBoolean("!undetectable", true);

  ExpectObject("true&&undetectable", obj);
  ExpectBoolean("false&&undetectable", false);
  ExpectBoolean("true||undetectable", true);
  ExpectObject("false||undetectable", obj);

  ExpectObject("undetectable&&true", obj);
  ExpectObject("undetectable&&false", obj);
  ExpectBoolean("undetectable||true", true);
  ExpectBoolean("undetectable||false", false);

  ExpectBoolean("undetectable==null", true);
  ExpectBoolean("null==undetectable", true);
  ExpectBoolean("undetectable==undefined", true);
  ExpectBoolean("undefined==undetectable", true);
  ExpectBoolean("undetectable==undetectable", true);


  ExpectBoolean("undetectable===null", false);
  ExpectBoolean("null===undetectable", false);
  ExpectBoolean("undetectable===undefined", false);
  ExpectBoolean("undefined===undetectable", false);
  ExpectBoolean("undetectable===undetectable", true);
}


THREADED_TEST(VoidLiteral) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);

  Local<v8::FunctionTemplate> desc = v8::FunctionTemplate::New(isolate);
  desc->InstanceTemplate()->MarkAsUndetectable();  // undetectable

  Local<v8::Object> obj = desc->GetFunction()->NewInstance();
  env->Global()->Set(v8_str("undetectable"), obj);

  ExpectBoolean("undefined == void 0", true);
  ExpectBoolean("undetectable == void 0", true);
  ExpectBoolean("null == void 0", true);
  ExpectBoolean("undefined === void 0", true);
  ExpectBoolean("undetectable === void 0", false);
  ExpectBoolean("null === void 0", false);

  ExpectBoolean("void 0 == undefined", true);
  ExpectBoolean("void 0 == undetectable", true);
  ExpectBoolean("void 0 == null", true);
  ExpectBoolean("void 0 === undefined", true);
  ExpectBoolean("void 0 === undetectable", false);
  ExpectBoolean("void 0 === null", false);

  ExpectString(
      "(function() {"
      "  try {"
      "    return x === void 0;"
      "  } catch(e) {"
      "    return e.toString();"
      "  }"
      "})()",
      "ReferenceError: x is not defined");
  ExpectString(
      "(function() {"
      "  try {"
      "    return void 0 === x;"
      "  } catch(e) {"
      "    return e.toString();"
      "  }"
      "})()",
      "ReferenceError: x is not defined");
}


THREADED_TEST(ExtensibleOnUndetectable) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);

  Local<v8::FunctionTemplate> desc = v8::FunctionTemplate::New(isolate);
  desc->InstanceTemplate()->MarkAsUndetectable();  // undetectable

  Local<v8::Object> obj = desc->GetFunction()->NewInstance();
  env->Global()->Set(v8_str("undetectable"), obj);

  Local<String> source = v8_str(
      "undetectable.x = 42;"
      "undetectable.x");

  Local<Script> script = v8_compile(source);

  CHECK(v8::Integer::New(isolate, 42)->Equals(script->Run()));

  ExpectBoolean("Object.isExtensible(undetectable)", true);

  source = v8_str("Object.preventExtensions(undetectable);");
  script = v8_compile(source);
  script->Run();
  ExpectBoolean("Object.isExtensible(undetectable)", false);

  source = v8_str("undetectable.y = 2000;");
  script = v8_compile(source);
  script->Run();
  ExpectBoolean("undetectable.y == undefined", true);
}


// The point of this test is type checking. We run it only so compilers
// don't complain about an unused function.
TEST(PersistentHandles) {
  LocalContext env;
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  Local<String> str = v8_str("foo");
  v8::Persistent<String> p_str(isolate, str);
  p_str.Reset();
  Local<Script> scr = v8_compile("");
  v8::Persistent<Script> p_scr(isolate, scr);
  p_scr.Reset();
  Local<ObjectTemplate> templ = ObjectTemplate::New(isolate);
  v8::Persistent<ObjectTemplate> p_templ(isolate, templ);
  p_templ.Reset();
}


static void HandleLogDelegator(
    const v8::FunctionCallbackInfo<v8::Value>& args) {
  ApiTestFuzzer::Fuzz();
}


THREADED_TEST(GlobalObjectTemplate) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope handle_scope(isolate);
  Local<ObjectTemplate> global_template = ObjectTemplate::New(isolate);
  global_template->Set(v8_str("JSNI_Log"),
                       v8::FunctionTemplate::New(isolate, HandleLogDelegator));
  v8::Local<Context> context = Context::New(isolate, 0, global_template);
  Context::Scope context_scope(context);
  CompileRun("JSNI_Log('LOG')");
}


static const char* kSimpleExtensionSource =
    "function Foo() {"
    "  return 4;"
    "}";


TEST(SimpleExtensions) {
  v8::HandleScope handle_scope(CcTest::isolate());
  v8::RegisterExtension(new Extension("simpletest", kSimpleExtensionSource));
  const char* extension_names[] = {"simpletest"};
  v8::ExtensionConfiguration extensions(1, extension_names);
  v8::Handle<Context> context = Context::New(CcTest::isolate(), &extensions);
  Context::Scope lock(context);
  v8::Handle<Value> result = CompileRun("Foo()");
  CHECK(result->Equals(v8::Integer::New(CcTest::isolate(), 4)));
}


static const char* kStackTraceFromExtensionSource =
    "function foo() {"
    "  throw new Error();"
    "}"
    "function bar() {"
    "  foo();"
    "}";


TEST(StackTraceInExtension) {
  v8::HandleScope handle_scope(CcTest::isolate());
  v8::RegisterExtension(
      new Extension("stacktracetest", kStackTraceFromExtensionSource));
  const char* extension_names[] = {"stacktracetest"};
  v8::ExtensionConfiguration extensions(1, extension_names);
  v8::Handle<Context> context = Context::New(CcTest::isolate(), &extensions);
  Context::Scope lock(context);
  CompileRun(
      "function user() { bar(); }"
      "var error;"
      "try{ user(); } catch (e) { error = e; }");
  CHECK_EQ(-1, CompileRun("error.stack.indexOf('foo')")->Int32Value());
  CHECK_EQ(-1, CompileRun("error.stack.indexOf('bar')")->Int32Value());
  CHECK_NE(-1, CompileRun("error.stack.indexOf('user')")->Int32Value());
}


TEST(NullExtensions) {
  v8::HandleScope handle_scope(CcTest::isolate());
  v8::RegisterExtension(new Extension("nulltest", NULL));
  const char* extension_names[] = {"nulltest"};
  v8::ExtensionConfiguration extensions(1, extension_names);
  v8::Handle<Context> context = Context::New(CcTest::isolate(), &extensions);
  Context::Scope lock(context);
  v8::Handle<Value> result = CompileRun("1+3");
  CHECK(result->Equals(v8::Integer::New(CcTest::isolate(), 4)));
}


static const char* kEmbeddedExtensionSource =
    "function Ret54321(){return 54321;}~~@@$"
    "$%% THIS IS A SERIES OF NON-NULL-TERMINATED STRINGS.";
static const int kEmbeddedExtensionSourceValidLen = 34;


TEST(ExtensionMissingSourceLength) {
  v8::HandleScope handle_scope(CcTest::isolate());
  v8::RegisterExtension(
      new Extension("srclentest_fail", kEmbeddedExtensionSource));
  const char* extension_names[] = {"srclentest_fail"};
  v8::ExtensionConfiguration extensions(1, extension_names);
  v8::Handle<Context> context = Context::New(CcTest::isolate(), &extensions);
  CHECK(0 == *context);
}


TEST(ExtensionWithSourceLength) {
  for (int source_len = kEmbeddedExtensionSourceValidLen - 1;
       source_len <= kEmbeddedExtensionSourceValidLen + 1; ++source_len) {
    v8::HandleScope handle_scope(CcTest::isolate());
    i::ScopedVector<char> extension_name(32);
    i::SNPrintF(extension_name, "ext #%d", source_len);
    v8::RegisterExtension(new Extension(
        extension_name.start(), kEmbeddedExtensionSource, 0, 0, source_len));
    const char* extension_names[1] = {extension_name.start()};
    v8::ExtensionConfiguration extensions(1, extension_names);
    v8::Handle<Context> context = Context::New(CcTest::isolate(), &extensions);
    if (source_len == kEmbeddedExtensionSourceValidLen) {
      Context::Scope lock(context);
      v8::Handle<Value> result = CompileRun("Ret54321()");
      CHECK(v8::Integer::New(CcTest::isolate(), 54321)->Equals(result));
    } else {
      // Anything but exactly the right length should fail to compile.
      CHECK(0 == *context);
    }
  }
}


static const char* kEvalExtensionSource1 =
    "function UseEval1() {"
    "  var x = 42;"
    "  return eval('x');"
    "}";


static const char* kEvalExtensionSource2 =
    "(function() {"
    "  var x = 42;"
    "  function e() {"
    "    return eval('x');"
    "  }"
    "  this.UseEval2 = e;"
    "})()";


TEST(UseEvalFromExtension) {
  v8::HandleScope handle_scope(CcTest::isolate());
  v8::RegisterExtension(new Extension("evaltest1", kEvalExtensionSource1));
  v8::RegisterExtension(new Extension("evaltest2", kEvalExtensionSource2));
  const char* extension_names[] = {"evaltest1", "evaltest2"};
  v8::ExtensionConfiguration extensions(2, extension_names);
  v8::Handle<Context> context = Context::New(CcTest::isolate(), &extensions);
  Context::Scope lock(context);
  v8::Handle<Value> result = CompileRun("UseEval1()");
  CHECK(result->Equals(v8::Integer::New(CcTest::isolate(), 42)));
  result = CompileRun("UseEval2()");
  CHECK(result->Equals(v8::Integer::New(CcTest::isolate(), 42)));
}


static const char* kWithExtensionSource1 =
    "function UseWith1() {"
    "  var x = 42;"
    "  with({x:87}) { return x; }"
    "}";


static const char* kWithExtensionSource2 =
    "(function() {"
    "  var x = 42;"
    "  function e() {"
    "    with ({x:87}) { return x; }"
    "  }"
    "  this.UseWith2 = e;"
    "})()";


TEST(UseWithFromExtension) {
  v8::HandleScope handle_scope(CcTest::isolate());
  v8::RegisterExtension(new Extension("withtest1", kWithExtensionSource1));
  v8::RegisterExtension(new Extension("withtest2", kWithExtensionSource2));
  const char* extension_names[] = {"withtest1", "withtest2"};
  v8::ExtensionConfiguration extensions(2, extension_names);
  v8::Handle<Context> context = Context::New(CcTest::isolate(), &extensions);
  Context::Scope lock(context);
  v8::Handle<Value> result = CompileRun("UseWith1()");
  CHECK(result->Equals(v8::Integer::New(CcTest::isolate(), 87)));
  result = CompileRun("UseWith2()");
  CHECK(result->Equals(v8::Integer::New(CcTest::isolate(), 87)));
}


TEST(AutoExtensions) {
  v8::HandleScope handle_scope(CcTest::isolate());
  Extension* extension = new Extension("autotest", kSimpleExtensionSource);
  extension->set_auto_enable(true);
  v8::RegisterExtension(extension);
  v8::Handle<Context> context = Context::New(CcTest::isolate());
  Context::Scope lock(context);
  v8::Handle<Value> result = CompileRun("Foo()");
  CHECK(result->Equals(v8::Integer::New(CcTest::isolate(), 4)));
}


static const char* kSyntaxErrorInExtensionSource = "[";


// Test that a syntax error in an extension does not cause a fatal
// error but results in an empty context.
TEST(SyntaxErrorExtensions) {
  v8::HandleScope handle_scope(CcTest::isolate());
  v8::RegisterExtension(
      new Extension("syntaxerror", kSyntaxErrorInExtensionSource));
  const char* extension_names[] = {"syntaxerror"};
  v8::ExtensionConfiguration extensions(1, extension_names);
  v8::Handle<Context> context = Context::New(CcTest::isolate(), &extensions);
  CHECK(context.IsEmpty());
}


static const char* kExceptionInExtensionSource = "throw 42";


// Test that an exception when installing an extension does not cause
// a fatal error but results in an empty context.
TEST(ExceptionExtensions) {
  v8::HandleScope handle_scope(CcTest::isolate());
  v8::RegisterExtension(
      new Extension("exception", kExceptionInExtensionSource));
  const char* extension_names[] = {"exception"};
  v8::ExtensionConfiguration extensions(1, extension_names);
  v8::Handle<Context> context = Context::New(CcTest::isolate(), &extensions);
  CHECK(context.IsEmpty());
}


static const char* kNativeCallInExtensionSource =
    "function call_runtime_last_index_of(x) {"
    "  return %StringLastIndexOf(x, 'bob', 10);"
    "}";


static const char* kNativeCallTest =
    "call_runtime_last_index_of('bobbobboellebobboellebobbob');";

// Test that a native runtime calls are supported in extensions.
TEST(NativeCallInExtensions) {
  v8::HandleScope handle_scope(CcTest::isolate());
  v8::RegisterExtension(
      new Extension("nativecall", kNativeCallInExtensionSource));
  const char* extension_names[] = {"nativecall"};
  v8::ExtensionConfiguration extensions(1, extension_names);
  v8::Handle<Context> context = Context::New(CcTest::isolate(), &extensions);
  Context::Scope lock(context);
  v8::Handle<Value> result = CompileRun(kNativeCallTest);
  CHECK(result->Equals(v8::Integer::New(CcTest::isolate(), 3)));
}


class NativeFunctionExtension : public Extension {
 public:
  NativeFunctionExtension(const char* name, const char* source,
                          v8::FunctionCallback fun = &Echo)
      : Extension(name, source), function_(fun) {}

  virtual v8::Handle<v8::FunctionTemplate> GetNativeFunctionTemplate(
      v8::Isolate* isolate, v8::Handle<v8::String> name) {
    return v8::FunctionTemplate::New(isolate, function_);
  }

  static void Echo(const v8::FunctionCallbackInfo<v8::Value>& args) {
    if (args.Length() >= 1) args.GetReturnValue().Set(args[0]);
  }

 private:
  v8::FunctionCallback function_;
};


TEST(NativeFunctionDeclaration) {
  v8::HandleScope handle_scope(CcTest::isolate());
  const char* name = "nativedecl";
  v8::RegisterExtension(
      new NativeFunctionExtension(name, "native function foo();"));
  const char* extension_names[] = {name};
  v8::ExtensionConfiguration extensions(1, extension_names);
  v8::Handle<Context> context = Context::New(CcTest::isolate(), &extensions);
  Context::Scope lock(context);
  v8::Handle<Value> result = CompileRun("foo(42);");
  CHECK(result->Equals(v8::Integer::New(CcTest::isolate(), 42)));
}


TEST(NativeFunctionDeclarationError) {
  v8::HandleScope handle_scope(CcTest::isolate());
  const char* name = "nativedeclerr";
  // Syntax error in extension code.
  v8::RegisterExtension(
      new NativeFunctionExtension(name, "native\nfunction foo();"));
  const char* extension_names[] = {name};
  v8::ExtensionConfiguration extensions(1, extension_names);
  v8::Handle<Context> context = Context::New(CcTest::isolate(), &extensions);
  CHECK(context.IsEmpty());
}


TEST(NativeFunctionDeclarationErrorEscape) {
  v8::HandleScope handle_scope(CcTest::isolate());
  const char* name = "nativedeclerresc";
  // Syntax error in extension code - escape code in "native" means that
  // it's not treated as a keyword.
  v8::RegisterExtension(
      new NativeFunctionExtension(name, "nativ\\u0065 function foo();"));
  const char* extension_names[] = {name};
  v8::ExtensionConfiguration extensions(1, extension_names);
  v8::Handle<Context> context = Context::New(CcTest::isolate(), &extensions);
  CHECK(context.IsEmpty());
}


static void CheckDependencies(const char* name, const char* expected) {
  v8::HandleScope handle_scope(CcTest::isolate());
  v8::ExtensionConfiguration config(1, &name);
  LocalContext context(&config);
  CHECK(String::NewFromUtf8(CcTest::isolate(), expected)
            ->Equals(context->Global()->Get(v8_str("loaded"))));
}


/*
 * Configuration:
 *
 *     /-- B <--\
 * A <-          -- D <-- E
 *     \-- C <--/
 */
THREADED_TEST(ExtensionDependency) {
  static const char* kEDeps[] = {"D"};
  v8::RegisterExtension(new Extension("E", "this.loaded += 'E';", 1, kEDeps));
  static const char* kDDeps[] = {"B", "C"};
  v8::RegisterExtension(new Extension("D", "this.loaded += 'D';", 2, kDDeps));
  static const char* kBCDeps[] = {"A"};
  v8::RegisterExtension(new Extension("B", "this.loaded += 'B';", 1, kBCDeps));
  v8::RegisterExtension(new Extension("C", "this.loaded += 'C';", 1, kBCDeps));
  v8::RegisterExtension(new Extension("A", "this.loaded += 'A';"));
  CheckDependencies("A", "undefinedA");
  CheckDependencies("B", "undefinedAB");
  CheckDependencies("C", "undefinedAC");
  CheckDependencies("D", "undefinedABCD");
  CheckDependencies("E", "undefinedABCDE");
  v8::HandleScope handle_scope(CcTest::isolate());
  static const char* exts[2] = {"C", "E"};
  v8::ExtensionConfiguration config(2, exts);
  LocalContext context(&config);
  CHECK(v8_str("undefinedACBDE")
            ->Equals(context->Global()->Get(v8_str("loaded"))));
}


static const char* kExtensionTestScript =
    "native function A();"
    "native function B();"
    "native function C();"
    "function Foo(i) {"
    "  if (i == 0) return A();"
    "  if (i == 1) return B();"
    "  if (i == 2) return C();"
    "}";


static void CallFun(const v8::FunctionCallbackInfo<v8::Value>& args) {
  ApiTestFuzzer::Fuzz();
  if (args.IsConstructCall()) {
    args.This()->Set(v8_str("data"), args.Data());
    args.GetReturnValue().SetNull();
    return;
  }
  args.GetReturnValue().Set(args.Data());
}


class FunctionExtension : public Extension {
 public:
  FunctionExtension() : Extension("functiontest", kExtensionTestScript) {}
  virtual v8::Handle<v8::FunctionTemplate> GetNativeFunctionTemplate(
      v8::Isolate* isolate, v8::Handle<String> name);
};


static int lookup_count = 0;
v8::Handle<v8::FunctionTemplate> FunctionExtension::GetNativeFunctionTemplate(
    v8::Isolate* isolate, v8::Handle<String> name) {
  lookup_count++;
  if (name->Equals(v8_str("A"))) {
    return v8::FunctionTemplate::New(isolate, CallFun,
                                     v8::Integer::New(isolate, 8));
  } else if (name->Equals(v8_str("B"))) {
    return v8::FunctionTemplate::New(isolate, CallFun,
                                     v8::Integer::New(isolate, 7));
  } else if (name->Equals(v8_str("C"))) {
    return v8::FunctionTemplate::New(isolate, CallFun,
                                     v8::Integer::New(isolate, 6));
  } else {
    return v8::Handle<v8::FunctionTemplate>();
  }
}


THREADED_TEST(FunctionLookup) {
  v8::RegisterExtension(new FunctionExtension());
  v8::HandleScope handle_scope(CcTest::isolate());
  static const char* exts[1] = {"functiontest"};
  v8::ExtensionConfiguration config(1, exts);
  LocalContext context(&config);
  CHECK_EQ(3, lookup_count);
  CHECK(v8::Integer::New(CcTest::isolate(), 8)->Equals(CompileRun("Foo(0)")));
  CHECK(v8::Integer::New(CcTest::isolate(), 7)->Equals(CompileRun("Foo(1)")));
  CHECK(v8::Integer::New(CcTest::isolate(), 6)->Equals(CompileRun("Foo(2)")));
}


THREADED_TEST(NativeFunctionConstructCall) {
  v8::RegisterExtension(new FunctionExtension());
  v8::HandleScope handle_scope(CcTest::isolate());
  static const char* exts[1] = {"functiontest"};
  v8::ExtensionConfiguration config(1, exts);
  LocalContext context(&config);
  for (int i = 0; i < 10; i++) {
    // Run a few times to ensure that allocation of objects doesn't
    // change behavior of a constructor function.
    CHECK(v8::Integer::New(CcTest::isolate(), 8)
              ->Equals(CompileRun("(new A()).data")));
    CHECK(v8::Integer::New(CcTest::isolate(), 7)
              ->Equals(CompileRun("(new B()).data")));
    CHECK(v8::Integer::New(CcTest::isolate(), 6)
              ->Equals(CompileRun("(new C()).data")));
  }
}


static const char* last_location;
static const char* last_message;
void StoringErrorCallback(const char* location, const char* message) {
  if (last_location == NULL) {
    last_location = location;
    last_message = message;
  }
}


// ErrorReporting creates a circular extensions configuration and
// tests that the fatal error handler gets called.  This renders V8
// unusable and therefore this test cannot be run in parallel.
TEST(ErrorReporting) {
  v8::V8::SetFatalErrorHandler(StoringErrorCallback);
  static const char* aDeps[] = {"B"};
  v8::RegisterExtension(new Extension("A", "", 1, aDeps));
  static const char* bDeps[] = {"A"};
  v8::RegisterExtension(new Extension("B", "", 1, bDeps));
  last_location = NULL;
  v8::ExtensionConfiguration config(1, bDeps);
  v8::Handle<Context> context = Context::New(CcTest::isolate(), &config);
  CHECK(context.IsEmpty());
  CHECK(last_location);
}


static void MissingScriptInfoMessageListener(v8::Handle<v8::Message> message,
                                             v8::Handle<Value> data) {
  CHECK(message->GetScriptOrigin().ResourceName()->IsUndefined());
  CHECK(v8::Undefined(CcTest::isolate())
            ->Equals(message->GetScriptOrigin().ResourceName()));
  message->GetLineNumber();
  message->GetSourceLine();
}


THREADED_TEST(ErrorWithMissingScriptInfo) {
  LocalContext context;
  v8::HandleScope scope(context->GetIsolate());
  v8::V8::AddMessageListener(MissingScriptInfoMessageListener);
  CompileRun("throw Error()");
  v8::V8::RemoveMessageListeners(MissingScriptInfoMessageListener);
}


struct FlagAndPersistent {
  bool flag;
  v8::Global<v8::Object> handle;
};


static void SetFlag(const v8::WeakCallbackInfo<FlagAndPersistent>& data) {
  data.GetParameter()->flag = true;
  data.GetParameter()->handle.Reset();
}


static void IndependentWeakHandle(bool global_gc, bool interlinked) {
  v8::Isolate* iso = CcTest::isolate();
  v8::HandleScope scope(iso);
  v8::Handle<Context> context = Context::New(iso);
  Context::Scope context_scope(context);

  FlagAndPersistent object_a, object_b;

  intptr_t big_heap_size;

  {
    v8::HandleScope handle_scope(iso);
    Local<Object> a(v8::Object::New(iso));
    Local<Object> b(v8::Object::New(iso));
    object_a.handle.Reset(iso, a);
    object_b.handle.Reset(iso, b);
    if (interlinked) {
      a->Set(v8_str("x"), b);
      b->Set(v8_str("x"), a);
    }
    if (global_gc) {
      CcTest::heap()->CollectAllGarbage(TestHeap::Heap::kNoGCFlags);
    } else {
      CcTest::heap()->CollectGarbage(i::NEW_SPACE);
    }
    // We are relying on this creating a big flag array and reserving the space
    // up front.
    v8::Handle<Value> big_array = CompileRun("new Array(50000)");
    a->Set(v8_str("y"), big_array);
    big_heap_size = CcTest::heap()->SizeOfObjects();
  }

  object_a.flag = false;
  object_b.flag = false;
  object_a.handle.SetWeak(&object_a, &SetFlag,
                          v8::WeakCallbackType::kParameter);
  object_b.handle.SetWeak(&object_b, &SetFlag,
                          v8::WeakCallbackType::kParameter);
  CHECK(!object_b.handle.IsIndependent());
  object_a.handle.MarkIndependent();
  object_b.handle.MarkIndependent();
  CHECK(object_b.handle.IsIndependent());
  if (global_gc) {
    CcTest::heap()->CollectAllGarbage(TestHeap::Heap::kNoGCFlags);
  } else {
    CcTest::heap()->CollectGarbage(i::NEW_SPACE);
  }
  // A single GC should be enough to reclaim the memory, since we are using
  // phantom handles.
  CHECK_LT(CcTest::heap()->SizeOfObjects(), big_heap_size - 200000);
  CHECK(object_a.flag);
  CHECK(object_b.flag);
}


THREADED_TEST(IndependentWeakHandle) {
  IndependentWeakHandle(false, false);
  IndependentWeakHandle(false, true);
  IndependentWeakHandle(true, false);
  IndependentWeakHandle(true, true);
}


class Trivial {
 public:
  explicit Trivial(int x) : x_(x) {}

  int x() { return x_; }
  void set_x(int x) { x_ = x; }

 private:
  int x_;
};


class Trivial2 {
 public:
  Trivial2(int x, int y) : y_(y), x_(x) {}

  int x() { return x_; }
  void set_x(int x) { x_ = x; }

  int y() { return y_; }
  void set_y(int y) { y_ = y; }

 private:
  int y_;
  int x_;
};


void CheckInternalFields(
    const v8::WeakCallbackInfo<v8::Persistent<v8::Object>>& data) {
  v8::Persistent<v8::Object>* handle = data.GetParameter();
  handle->Reset();
  Trivial* t1 = reinterpret_cast<Trivial*>(data.GetInternalField1());
  Trivial2* t2 = reinterpret_cast<Trivial2*>(data.GetInternalField2());
  CHECK_EQ(42, t1->x());
  CHECK_EQ(103, t2->x());
  t1->set_x(1729);
  t2->set_x(33550336);
}


void InternalFieldCallback(bool global_gc) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);

  Local<v8::FunctionTemplate> templ = v8::FunctionTemplate::New(isolate);
  Local<v8::ObjectTemplate> instance_templ = templ->InstanceTemplate();
  Trivial* t1;
  Trivial2* t2;
  instance_templ->SetInternalFieldCount(2);
  {
    v8::HandleScope scope(isolate);
    Local<v8::Object> obj = templ->GetFunction()->NewInstance();
    v8::Persistent<v8::Object> handle(isolate, obj);
    CHECK_EQ(2, obj->InternalFieldCount());
    CHECK(obj->GetInternalField(0)->IsUndefined());
    t1 = new Trivial(42);
    t2 = new Trivial2(103, 9);

    obj->SetAlignedPointerInInternalField(0, t1);
    t1 = reinterpret_cast<Trivial*>(obj->GetAlignedPointerFromInternalField(0));
    CHECK_EQ(42, t1->x());

    obj->SetAlignedPointerInInternalField(1, t2);
    t2 =
        reinterpret_cast<Trivial2*>(obj->GetAlignedPointerFromInternalField(1));
    CHECK_EQ(103, t2->x());

    handle.SetWeak<v8::Persistent<v8::Object>>(
        &handle, CheckInternalFields, v8::WeakCallbackType::kInternalFields);
    if (!global_gc) {
      handle.MarkIndependent();
    }
  }
  if (global_gc) {
    CcTest::heap()->CollectAllGarbage(TestHeap::Heap::kNoGCFlags);
  } else {
    CcTest::heap()->CollectGarbage(i::NEW_SPACE);
  }

  CHECK_EQ(1729, t1->x());
  CHECK_EQ(33550336, t2->x());

  delete t1;
  delete t2;
}


THREADED_TEST(InternalFieldCallback) {
  InternalFieldCallback(false);
  InternalFieldCallback(true);
}


static void ResetUseValueAndSetFlag(
    const v8::WeakCallbackData<v8::Object, FlagAndPersistent>& data) {
  // Blink will reset the handle, and then use the other handle, so they
  // can't use the same backing slot.
  data.GetParameter()->handle.Reset();
  data.GetValue()->IsBoolean();  // Make sure the handle still works.
  data.GetParameter()->flag = true;
}


static void ResetWeakHandle(bool global_gc) {
  v8::Isolate* iso = CcTest::isolate();
  v8::HandleScope scope(iso);
  v8::Handle<Context> context = Context::New(iso);
  Context::Scope context_scope(context);

  FlagAndPersistent object_a, object_b;

  {
    v8::HandleScope handle_scope(iso);
    Local<Object> a(v8::Object::New(iso));
    Local<Object> b(v8::Object::New(iso));
    object_a.handle.Reset(iso, a);
    object_b.handle.Reset(iso, b);
    if (global_gc) {
      CcTest::heap()->CollectAllGarbage(
          TestHeap::Heap::kAbortIncrementalMarkingMask);
    } else {
      CcTest::heap()->CollectGarbage(i::NEW_SPACE);
    }
  }

  object_a.flag = false;
  object_b.flag = false;
  object_a.handle.SetWeak(&object_a, &ResetUseValueAndSetFlag);
  object_b.handle.SetWeak(&object_b, &ResetUseValueAndSetFlag);
  if (!global_gc) {
    object_a.handle.MarkIndependent();
    object_b.handle.MarkIndependent();
    CHECK(object_b.handle.IsIndependent());
  }
  if (global_gc) {
    CcTest::heap()->CollectAllGarbage(
        TestHeap::Heap::kAbortIncrementalMarkingMask);
  } else {
    CcTest::heap()->CollectGarbage(i::NEW_SPACE);
  }
  CHECK(object_a.flag);
  CHECK(object_b.flag);
}


THREADED_TEST(ResetWeakHandle) {
  ResetWeakHandle(false);
  ResetWeakHandle(true);
}


static void InvokeScavenge() { CcTest::heap()->CollectGarbage(i::NEW_SPACE); }


static void InvokeMarkSweep() {
  CcTest::heap()->CollectAllGarbage(i::Heap::kNoGCFlags);
}


static void ForceScavenge(
    const v8::WeakCallbackData<v8::Object, FlagAndPersistent>& data) {
  data.GetParameter()->handle.Reset();
  data.GetParameter()->flag = true;
  InvokeScavenge();
}


static void ForceMarkSweep(
    const v8::WeakCallbackData<v8::Object, FlagAndPersistent>& data) {
  data.GetParameter()->handle.Reset();
  data.GetParameter()->flag = true;
  InvokeMarkSweep();
}


THREADED_TEST(GCFromWeakCallbacks) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  v8::Handle<Context> context = Context::New(isolate);
  Context::Scope context_scope(context);

  static const int kNumberOfGCTypes = 2;
  typedef v8::WeakCallbackData<v8::Object, FlagAndPersistent>::Callback
      Callback;
  Callback gc_forcing_callback[kNumberOfGCTypes] =
      {&ForceScavenge, &ForceMarkSweep};

  typedef void (*GCInvoker)();
  GCInvoker invoke_gc[kNumberOfGCTypes] = {&InvokeScavenge, &InvokeMarkSweep};

  for (int outer_gc = 0; outer_gc < kNumberOfGCTypes; outer_gc++) {
    for (int inner_gc = 0; inner_gc < kNumberOfGCTypes; inner_gc++) {
      FlagAndPersistent object;
      {
        v8::HandleScope handle_scope(isolate);
        object.handle.Reset(isolate, v8::Object::New(isolate));
      }
      object.flag = false;
      object.handle.SetWeak(&object, gc_forcing_callback[inner_gc]);
      object.handle.MarkIndependent();
      invoke_gc[outer_gc]();
      CHECK(object.flag);
    }
  }
}


static void RevivingCallback(
    const v8::WeakCallbackData<v8::Object, FlagAndPersistent>& data) {
  data.GetParameter()->handle.ClearWeak();
  data.GetParameter()->flag = true;
}


THREADED_TEST(IndependentHandleRevival) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  v8::Handle<Context> context = Context::New(isolate);
  Context::Scope context_scope(context);

  FlagAndPersistent object;
  {
    v8::HandleScope handle_scope(isolate);
    v8::Local<v8::Object> o = v8::Object::New(isolate);
    object.handle.Reset(isolate, o);
    o->Set(v8_str("x"), v8::Integer::New(isolate, 1));
    v8::Local<String> y_str = v8_str("y");
    o->Set(y_str, y_str);
  }
  object.flag = false;
  object.handle.SetWeak(&object, &RevivingCallback);
  object.handle.MarkIndependent();
  CcTest::heap()->CollectGarbage(i::NEW_SPACE);
  CHECK(object.flag);
  CcTest::heap()->CollectAllGarbage(i::Heap::kAbortIncrementalMarkingMask);
  {
    v8::HandleScope handle_scope(isolate);
    v8::Local<v8::Object> o =
        v8::Local<v8::Object>::New(isolate, object.handle);
    v8::Local<String> y_str = v8_str("y");
    CHECK(v8::Integer::New(isolate, 1)->Equals(o->Get(v8_str("x"))));
    CHECK(o->Get(y_str)->Equals(y_str));
  }
}


v8::Handle<Function> args_fun;


static void ArgumentsTestCallback(
    const v8::FunctionCallbackInfo<v8::Value>& args) {
  ApiTestFuzzer::Fuzz();
  v8::Isolate* isolate = args.GetIsolate();
  CHECK(args_fun->Equals(args.Callee()));
  CHECK_EQ(3, args.Length());
  CHECK(v8::Integer::New(isolate, 1)->Equals(args[0]));
  CHECK(v8::Integer::New(isolate, 2)->Equals(args[1]));
  CHECK(v8::Integer::New(isolate, 3)->Equals(args[2]));
  CHECK(v8::Undefined(isolate)->Equals(args[3]));
  v8::HandleScope scope(args.GetIsolate());
  CcTest::heap()->CollectAllGarbage(i::Heap::kNoGCFlags);
}


THREADED_TEST(Arguments) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  v8::Handle<v8::ObjectTemplate> global = ObjectTemplate::New(isolate);
  global->Set(v8_str("f"),
              v8::FunctionTemplate::New(isolate, ArgumentsTestCallback));
  LocalContext context(NULL, global);
  args_fun = context->Global()->Get(v8_str("f")).As<Function>();
  v8_compile("f(1, 2, 3)")->Run();
}


static int p_getter_count;
static int p_getter_count2;


static void PGetter(Local<String> name,
                    const v8::PropertyCallbackInfo<v8::Value>& info) {
  ApiTestFuzzer::Fuzz();
  p_getter_count++;
  v8::Handle<v8::Object> global =
      info.GetIsolate()->GetCurrentContext()->Global();
  CHECK(info.Holder()->Equals(global->Get(v8_str("o1"))));
  if (name->Equals(v8_str("p1"))) {
    CHECK(info.This()->Equals(global->Get(v8_str("o1"))));
  } else if (name->Equals(v8_str("p2"))) {
    CHECK(info.This()->Equals(global->Get(v8_str("o2"))));
  } else if (name->Equals(v8_str("p3"))) {
    CHECK(info.This()->Equals(global->Get(v8_str("o3"))));
  } else if (name->Equals(v8_str("p4"))) {
    CHECK(info.This()->Equals(global->Get(v8_str("o4"))));
  }
}


static void RunHolderTest(v8::Handle<v8::ObjectTemplate> obj) {
  ApiTestFuzzer::Fuzz();
  LocalContext context;
  context->Global()->Set(v8_str("o1"), obj->NewInstance());
  CompileRun(
    "o1.__proto__ = { };"
    "var o2 = { __proto__: o1 };"
    "var o3 = { __proto__: o2 };"
    "var o4 = { __proto__: o3 };"
    "for (var i = 0; i < 10; i++) o4.p4;"
    "for (var i = 0; i < 10; i++) o3.p3;"
    "for (var i = 0; i < 10; i++) o2.p2;"
    "for (var i = 0; i < 10; i++) o1.p1;");
}


static void PGetter2(Local<Name> name,
                     const v8::PropertyCallbackInfo<v8::Value>& info) {
  ApiTestFuzzer::Fuzz();
  p_getter_count2++;
  v8::Handle<v8::Object> global =
      info.GetIsolate()->GetCurrentContext()->Global();
  CHECK(info.Holder()->Equals(global->Get(v8_str("o1"))));
  if (name->Equals(v8_str("p1"))) {
    CHECK(info.This()->Equals(global->Get(v8_str("o1"))));
  } else if (name->Equals(v8_str("p2"))) {
    CHECK(info.This()->Equals(global->Get(v8_str("o2"))));
  } else if (name->Equals(v8_str("p3"))) {
    CHECK(info.This()->Equals(global->Get(v8_str("o3"))));
  } else if (name->Equals(v8_str("p4"))) {
    CHECK(info.This()->Equals(global->Get(v8_str("o4"))));
  }
}


THREADED_TEST(GetterHolders) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  v8::Handle<v8::ObjectTemplate> obj = ObjectTemplate::New(isolate);
  obj->SetAccessor(v8_str("p1"), PGetter);
  obj->SetAccessor(v8_str("p2"), PGetter);
  obj->SetAccessor(v8_str("p3"), PGetter);
  obj->SetAccessor(v8_str("p4"), PGetter);
  p_getter_count = 0;
  RunHolderTest(obj);
  CHECK_EQ(40, p_getter_count);
}


THREADED_TEST(PreInterceptorHolders) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  v8::Handle<v8::ObjectTemplate> obj = ObjectTemplate::New(isolate);
  obj->SetHandler(v8::NamedPropertyHandlerConfiguration(PGetter2));
  p_getter_count2 = 0;
  RunHolderTest(obj);
  CHECK_EQ(40, p_getter_count2);
}


THREADED_TEST(ObjectInstantiation) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  v8::Handle<v8::ObjectTemplate> templ = ObjectTemplate::New(isolate);
  templ->SetAccessor(v8_str("t"), PGetter2);
  LocalContext context;
  context->Global()->Set(v8_str("o"), templ->NewInstance());
  for (int i = 0; i < 100; i++) {
    v8::HandleScope inner_scope(CcTest::isolate());
    v8::Handle<v8::Object> obj = templ->NewInstance();
    CHECK(!obj->Equals(context->Global()->Get(v8_str("o"))));
    context->Global()->Set(v8_str("o2"), obj);
    v8::Handle<Value> value =
        CompileRun("o.__proto__ === o2.__proto__");
    CHECK(v8::True(isolate)->Equals(value));
    context->Global()->Set(v8_str("o"), obj);
  }
}


static int StrCmp16(uint16_t* a, uint16_t* b) {
  while (true) {
    if (*a == 0 && *b == 0) return 0;
    if (*a != *b) return 0 + *a - *b;
    a++;
    b++;
  }
}


static int StrNCmp16(uint16_t* a, uint16_t* b, int n) {
  while (true) {
    if (n-- == 0) return 0;
    if (*a == 0 && *b == 0) return 0;
    if (*a != *b) return 0 + *a - *b;
    a++;
    b++;
  }
}


int GetUtf8Length(Handle<String> str) {
  int len = str->Utf8Length();
  if (len < 0) {
    i::Handle<i::String> istr(v8::Utils::OpenHandle(*str));
    i::String::Flatten(istr);
    len = str->Utf8Length();
  }
  return len;
}


THREADED_TEST(StringWrite) {
  LocalContext context;
  v8::HandleScope scope(context->GetIsolate());
  v8::Handle<String> str = v8_str("abcde");
  // abc<Icelandic eth><Unicode snowman>.
  v8::Handle<String> str2 = v8_str("abc\303\260\342\230\203");
  v8::Handle<String> str3 = v8::String::NewFromUtf8(
      context->GetIsolate(), "abc\0def", v8::String::kNormalString, 7);
  // "ab" + lead surrogate + "cd" + trail surrogate + "ef"
  uint16_t orphans[8] = { 0x61, 0x62, 0xd800, 0x63, 0x64, 0xdc00, 0x65, 0x66 };
  v8::Handle<String> orphans_str = v8::String::NewFromTwoByte(
      context->GetIsolate(), orphans, v8::String::kNormalString, 8);
  // single lead surrogate
  uint16_t lead[1] = { 0xd800 };
  v8::Handle<String> lead_str = v8::String::NewFromTwoByte(
      context->GetIsolate(), lead, v8::String::kNormalString, 1);
  // single trail surrogate
  uint16_t trail[1] = { 0xdc00 };
  v8::Handle<String> trail_str = v8::String::NewFromTwoByte(
      context->GetIsolate(), trail, v8::String::kNormalString, 1);
  // surrogate pair
  uint16_t pair[2] = { 0xd800,  0xdc00 };
  v8::Handle<String> pair_str = v8::String::NewFromTwoByte(
      context->GetIsolate(), pair, v8::String::kNormalString, 2);
  const int kStride = 4;  // Must match stride in for loops in JS below.
  CompileRun(
      "var left = '';"
      "for (var i = 0; i < 0xd800; i += 4) {"
      "  left = left + String.fromCharCode(i);"
      "}");
  CompileRun(
      "var right = '';"
      "for (var i = 0; i < 0xd800; i += 4) {"
      "  right = String.fromCharCode(i) + right;"
      "}");
  v8::Handle<v8::Object> global = context->Global();
  Handle<String> left_tree = global->Get(v8_str("left")).As<String>();
  Handle<String> right_tree = global->Get(v8_str("right")).As<String>();

  CHECK_EQ(5, str2->Length());
  CHECK_EQ(0xd800 / kStride, left_tree->Length());
  CHECK_EQ(0xd800 / kStride, right_tree->Length());

  char buf[100];
  char utf8buf[0xd800 * 3];
  uint16_t wbuf[100];
  int len;
  int charlen;

  memset(utf8buf, 0x1, 1000);
  len = str2->WriteUtf8(utf8buf, sizeof(utf8buf), &charlen);
  CHECK_EQ(9, len);
  CHECK_EQ(5, charlen);
  CHECK_EQ(0, strcmp(utf8buf, "abc\303\260\342\230\203"));

  memset(utf8buf, 0x1, 1000);
  len = str2->WriteUtf8(utf8buf, 8, &charlen);
  CHECK_EQ(8, len);
  CHECK_EQ(5, charlen);
  CHECK_EQ(0, strncmp(utf8buf, "abc\303\260\342\230\203\1", 9));

  memset(utf8buf, 0x1, 1000);
  len = str2->WriteUtf8(utf8buf, 7, &charlen);
  CHECK_EQ(5, len);
  CHECK_EQ(4, charlen);
  CHECK_EQ(0, strncmp(utf8buf, "abc\303\260\1", 5));

  memset(utf8buf, 0x1, 1000);
  len = str2->WriteUtf8(utf8buf, 6, &charlen);
  CHECK_EQ(5, len);
  CHECK_EQ(4, charlen);
  CHECK_EQ(0, strncmp(utf8buf, "abc\303\260\1", 5));

  memset(utf8buf, 0x1, 1000);
  len = str2->WriteUtf8(utf8buf, 5, &charlen);
  CHECK_EQ(5, len);
  CHECK_EQ(4, charlen);
  CHECK_EQ(0, strncmp(utf8buf, "abc\303\260\1", 5));

  memset(utf8buf, 0x1, 1000);
  len = str2->WriteUtf8(utf8buf, 4, &charlen);
  CHECK_EQ(3, len);
  CHECK_EQ(3, charlen);
  CHECK_EQ(0, strncmp(utf8buf, "abc\1", 4));

  memset(utf8buf, 0x1, 1000);
  len = str2->WriteUtf8(utf8buf, 3, &charlen);
  CHECK_EQ(3, len);
  CHECK_EQ(3, charlen);
  CHECK_EQ(0, strncmp(utf8buf, "abc\1", 4));

  memset(utf8buf, 0x1, 1000);
  len = str2->WriteUtf8(utf8buf, 2, &charlen);
  CHECK_EQ(2, len);
  CHECK_EQ(2, charlen);
  CHECK_EQ(0, strncmp(utf8buf, "ab\1", 3));

  // allow orphan surrogates by default
  memset(utf8buf, 0x1, 1000);
  len = orphans_str->WriteUtf8(utf8buf, sizeof(utf8buf), &charlen);
  CHECK_EQ(13, len);
  CHECK_EQ(8, charlen);
  CHECK_EQ(0, strcmp(utf8buf, "ab\355\240\200cd\355\260\200ef"));

  // replace orphan surrogates with unicode replacement character
  memset(utf8buf, 0x1, 1000);
  len = orphans_str->WriteUtf8(utf8buf,
                               sizeof(utf8buf),
                               &charlen,
                               String::REPLACE_INVALID_UTF8);
  CHECK_EQ(13, len);
  CHECK_EQ(8, charlen);
  CHECK_EQ(0, strcmp(utf8buf, "ab\357\277\275cd\357\277\275ef"));

  // replace single lead surrogate with unicode replacement character
  memset(utf8buf, 0x1, 1000);
  len = lead_str->WriteUtf8(utf8buf,
                            sizeof(utf8buf),
                            &charlen,
                            String::REPLACE_INVALID_UTF8);
  CHECK_EQ(4, len);
  CHECK_EQ(1, charlen);
  CHECK_EQ(0, strcmp(utf8buf, "\357\277\275"));

  // replace single trail surrogate with unicode replacement character
  memset(utf8buf, 0x1, 1000);
  len = trail_str->WriteUtf8(utf8buf,
                             sizeof(utf8buf),
                             &charlen,
                             String::REPLACE_INVALID_UTF8);
  CHECK_EQ(4, len);
  CHECK_EQ(1, charlen);
  CHECK_EQ(0, strcmp(utf8buf, "\357\277\275"));

  // do not replace / write anything if surrogate pair does not fit the buffer
  // space
  memset(utf8buf, 0x1, 1000);
  len = pair_str->WriteUtf8(utf8buf,
                             3,
                             &charlen,
                             String::REPLACE_INVALID_UTF8);
  CHECK_EQ(0, len);
  CHECK_EQ(0, charlen);

  memset(utf8buf, 0x1, sizeof(utf8buf));
  len = GetUtf8Length(left_tree);
  int utf8_expected =
      (0x80 + (0x800 - 0x80) * 2 + (0xd800 - 0x800) * 3) / kStride;
  CHECK_EQ(utf8_expected, len);
  len = left_tree->WriteUtf8(utf8buf, utf8_expected, &charlen);
  CHECK_EQ(utf8_expected, len);
  CHECK_EQ(0xd800 / kStride, charlen);
  CHECK_EQ(0xed, static_cast<unsigned char>(utf8buf[utf8_expected - 3]));
  CHECK_EQ(0x9f, static_cast<unsigned char>(utf8buf[utf8_expected - 2]));
  CHECK_EQ(0xc0 - kStride,
           static_cast<unsigned char>(utf8buf[utf8_expected - 1]));
  CHECK_EQ(1, utf8buf[utf8_expected]);

  memset(utf8buf, 0x1, sizeof(utf8buf));
  len = GetUtf8Length(right_tree);
  CHECK_EQ(utf8_expected, len);
  len = right_tree->WriteUtf8(utf8buf, utf8_expected, &charlen);
  CHECK_EQ(utf8_expected, len);
  CHECK_EQ(0xd800 / kStride, charlen);
  CHECK_EQ(0xed, static_cast<unsigned char>(utf8buf[0]));
  CHECK_EQ(0x9f, static_cast<unsigned char>(utf8buf[1]));
  CHECK_EQ(0xc0 - kStride, static_cast<unsigned char>(utf8buf[2]));
  CHECK_EQ(1, utf8buf[utf8_expected]);

  memset(buf, 0x1, sizeof(buf));
  memset(wbuf, 0x1, sizeof(wbuf));
  len = str->WriteOneByte(reinterpret_cast<uint8_t*>(buf));
  CHECK_EQ(5, len);
  len = str->Write(wbuf);
  CHECK_EQ(5, len);
  CHECK_EQ(0, strcmp("abcde", buf));
  uint16_t answer1[] = {'a', 'b', 'c', 'd', 'e', '\0'};
  CHECK_EQ(0, StrCmp16(answer1, wbuf));

  memset(buf, 0x1, sizeof(buf));
  memset(wbuf, 0x1, sizeof(wbuf));
  len = str->WriteOneByte(reinterpret_cast<uint8_t*>(buf), 0, 4);
  CHECK_EQ(4, len);
  len = str->Write(wbuf, 0, 4);
  CHECK_EQ(4, len);
  CHECK_EQ(0, strncmp("abcd\1", buf, 5));
  uint16_t answer2[] = {'a', 'b', 'c', 'd', 0x101};
  CHECK_EQ(0, StrNCmp16(answer2, wbuf, 5));

  memset(buf, 0x1, sizeof(buf));
  memset(wbuf, 0x1, sizeof(wbuf));
  len = str->WriteOneByte(reinterpret_cast<uint8_t*>(buf), 0, 5);
  CHECK_EQ(5, len);
  len = str->Write(wbuf, 0, 5);
  CHECK_EQ(5, len);
  CHECK_EQ(0, strncmp("abcde\1", buf, 6));
  uint16_t answer3[] = {'a', 'b', 'c', 'd', 'e', 0x101};
  CHECK_EQ(0, StrNCmp16(answer3, wbuf, 6));

  memset(buf, 0x1, sizeof(buf));
  memset(wbuf, 0x1, sizeof(wbuf));
  len = str->WriteOneByte(reinterpret_cast<uint8_t*>(buf), 0, 6);
  CHECK_EQ(5, len);
  len = str->Write(wbuf, 0, 6);
  CHECK_EQ(5, len);
  CHECK_EQ(0, strcmp("abcde", buf));
  uint16_t answer4[] = {'a', 'b', 'c', 'd', 'e', '\0'};
  CHECK_EQ(0, StrCmp16(answer4, wbuf));

  memset(buf, 0x1, sizeof(buf));
  memset(wbuf, 0x1, sizeof(wbuf));
  len = str->WriteOneByte(reinterpret_cast<uint8_t*>(buf), 4, -1);
  CHECK_EQ(1, len);
  len = str->Write(wbuf, 4, -1);
  CHECK_EQ(1, len);
  CHECK_EQ(0, strcmp("e", buf));
  uint16_t answer5[] = {'e', '\0'};
  CHECK_EQ(0, StrCmp16(answer5, wbuf));

  memset(buf, 0x1, sizeof(buf));
  memset(wbuf, 0x1, sizeof(wbuf));
  len = str->WriteOneByte(reinterpret_cast<uint8_t*>(buf), 4, 6);
  CHECK_EQ(1, len);
  len = str->Write(wbuf, 4, 6);
  CHECK_EQ(1, len);
  CHECK_EQ(0, strcmp("e", buf));
  CHECK_EQ(0, StrCmp16(answer5, wbuf));

  memset(buf, 0x1, sizeof(buf));
  memset(wbuf, 0x1, sizeof(wbuf));
  len = str->WriteOneByte(reinterpret_cast<uint8_t*>(buf), 4, 1);
  CHECK_EQ(1, len);
  len = str->Write(wbuf, 4, 1);
  CHECK_EQ(1, len);
  CHECK_EQ(0, strncmp("e\1", buf, 2));
  uint16_t answer6[] = {'e', 0x101};
  CHECK_EQ(0, StrNCmp16(answer6, wbuf, 2));

  memset(buf, 0x1, sizeof(buf));
  memset(wbuf, 0x1, sizeof(wbuf));
  len = str->WriteOneByte(reinterpret_cast<uint8_t*>(buf), 3, 1);
  CHECK_EQ(1, len);
  len = str->Write(wbuf, 3, 1);
  CHECK_EQ(1, len);
  CHECK_EQ(0, strncmp("d\1", buf, 2));
  uint16_t answer7[] = {'d', 0x101};
  CHECK_EQ(0, StrNCmp16(answer7, wbuf, 2));

  memset(wbuf, 0x1, sizeof(wbuf));
  wbuf[5] = 'X';
  len = str->Write(wbuf, 0, 6, String::NO_NULL_TERMINATION);
  CHECK_EQ(5, len);
  CHECK_EQ('X', wbuf[5]);
  uint16_t answer8a[] = {'a', 'b', 'c', 'd', 'e'};
  uint16_t answer8b[] = {'a', 'b', 'c', 'd', 'e', '\0'};
  CHECK_EQ(0, StrNCmp16(answer8a, wbuf, 5));
  CHECK_NE(0, StrCmp16(answer8b, wbuf));
  wbuf[5] = '\0';
  CHECK_EQ(0, StrCmp16(answer8b, wbuf));

  memset(buf, 0x1, sizeof(buf));
  buf[5] = 'X';
  len = str->WriteOneByte(reinterpret_cast<uint8_t*>(buf),
                          0,
                          6,
                          String::NO_NULL_TERMINATION);
  CHECK_EQ(5, len);
  CHECK_EQ('X', buf[5]);
  CHECK_EQ(0, strncmp("abcde", buf, 5));
  CHECK_NE(0, strcmp("abcde", buf));
  buf[5] = '\0';
  CHECK_EQ(0, strcmp("abcde", buf));

  memset(utf8buf, 0x1, sizeof(utf8buf));
  utf8buf[8] = 'X';
  len = str2->WriteUtf8(utf8buf, sizeof(utf8buf), &charlen,
                        String::NO_NULL_TERMINATION);
  CHECK_EQ(8, len);
  CHECK_EQ('X', utf8buf[8]);
  CHECK_EQ(5, charlen);
  CHECK_EQ(0, strncmp(utf8buf, "abc\303\260\342\230\203", 8));
  CHECK_NE(0, strcmp(utf8buf, "abc\303\260\342\230\203"));
  utf8buf[8] = '\0';
  CHECK_EQ(0, strcmp(utf8buf, "abc\303\260\342\230\203"));

  memset(utf8buf, 0x1, sizeof(utf8buf));
  utf8buf[5] = 'X';
  len = str->WriteUtf8(utf8buf, sizeof(utf8buf), &charlen,
                        String::NO_NULL_TERMINATION);
  CHECK_EQ(5, len);
  CHECK_EQ('X', utf8buf[5]);  // Test that the sixth character is untouched.
  CHECK_EQ(5, charlen);
  utf8buf[5] = '\0';
  CHECK_EQ(0, strcmp(utf8buf, "abcde"));

  memset(buf, 0x1, sizeof(buf));
  len = str3->WriteOneByte(reinterpret_cast<uint8_t*>(buf));
  CHECK_EQ(7, len);
  CHECK_EQ(0, strcmp("abc", buf));
  CHECK_EQ(0, buf[3]);
  CHECK_EQ(0, strcmp("def", buf + 4));

  CHECK_EQ(0, str->WriteOneByte(NULL, 0, 0, String::NO_NULL_TERMINATION));
  CHECK_EQ(0, str->WriteUtf8(NULL, 0, 0, String::NO_NULL_TERMINATION));
  CHECK_EQ(0, str->Write(NULL, 0, 0, String::NO_NULL_TERMINATION));
}


static void Utf16Helper(
    LocalContext& context,  // NOLINT
    const char* name,
    const char* lengths_name,
    int len) {
  Local<v8::Array> a =
      Local<v8::Array>::Cast(context->Global()->Get(v8_str(name)));
  Local<v8::Array> alens =
      Local<v8::Array>::Cast(context->Global()->Get(v8_str(lengths_name)));
  for (int i = 0; i < len; i++) {
    Local<v8::String> string =
      Local<v8::String>::Cast(a->Get(i));
    Local<v8::Number> expected_len =
      Local<v8::Number>::Cast(alens->Get(i));
    int length = GetUtf8Length(string);
    CHECK_EQ(static_cast<int>(expected_len->Value()), length);
  }
}


static uint16_t StringGet(Handle<String> str, int index) {
  i::Handle<i::String> istring =
      v8::Utils::OpenHandle(String::Cast(*str));
  return istring->Get(index);
}


static void WriteUtf8Helper(
    LocalContext& context,  // NOLINT
    const char* name,
    const char* lengths_name,
    int len) {
  Local<v8::Array> b =
      Local<v8::Array>::Cast(context->Global()->Get(v8_str(name)));
  Local<v8::Array> alens =
      Local<v8::Array>::Cast(context->Global()->Get(v8_str(lengths_name)));
  char buffer[1000];
  char buffer2[1000];
  for (int i = 0; i < len; i++) {
    Local<v8::String> string =
      Local<v8::String>::Cast(b->Get(i));
    Local<v8::Number> expected_len =
      Local<v8::Number>::Cast(alens->Get(i));
    int utf8_length = static_cast<int>(expected_len->Value());
    for (int j = utf8_length + 1; j >= 0; j--) {
      memset(reinterpret_cast<void*>(&buffer), 42, sizeof(buffer));
      memset(reinterpret_cast<void*>(&buffer2), 42, sizeof(buffer2));
      int nchars;
      int utf8_written =
          string->WriteUtf8(buffer, j, &nchars, String::NO_OPTIONS);
      int utf8_written2 =
          string->WriteUtf8(buffer2, j, &nchars, String::NO_NULL_TERMINATION);
      CHECK_GE(utf8_length + 1, utf8_written);
      CHECK_GE(utf8_length, utf8_written2);
      for (int k = 0; k < utf8_written2; k++) {
        CHECK_EQ(buffer[k], buffer2[k]);
      }
      CHECK(nchars * 3 >= utf8_written - 1);
      CHECK(nchars <= utf8_written);
      if (j == utf8_length + 1) {
        CHECK_EQ(utf8_written2, utf8_length);
        CHECK_EQ(utf8_written2 + 1, utf8_written);
      }
      CHECK_EQ(buffer[utf8_written], 42);
      if (j > utf8_length) {
        if (utf8_written != 0) CHECK_EQ(buffer[utf8_written - 1], 0);
        if (utf8_written > 1) CHECK_NE(buffer[utf8_written - 2], 42);
        Handle<String> roundtrip = v8_str(buffer);
        CHECK(roundtrip->Equals(string));
      } else {
        if (utf8_written != 0) CHECK_NE(buffer[utf8_written - 1], 42);
      }
      if (utf8_written2 != 0) CHECK_NE(buffer[utf8_written - 1], 42);
      if (nchars >= 2) {
        uint16_t trail = StringGet(string, nchars - 1);
        uint16_t lead = StringGet(string, nchars - 2);
        if (((lead & 0xfc00) == 0xd800) &&
            ((trail & 0xfc00) == 0xdc00)) {
          unsigned u1 = buffer2[utf8_written2 - 4];
          unsigned u2 = buffer2[utf8_written2 - 3];
          unsigned u3 = buffer2[utf8_written2 - 2];
          unsigned u4 = buffer2[utf8_written2 - 1];
          CHECK_EQ((u1 & 0xf8), 0xf0u);
          CHECK_EQ((u2 & 0xc0), 0x80u);
          CHECK_EQ((u3 & 0xc0), 0x80u);
          CHECK_EQ((u4 & 0xc0), 0x80u);
          uint32_t c = 0x10000 + ((lead & 0x3ff) << 10) + (trail & 0x3ff);
          CHECK_EQ((u4 & 0x3f), (c & 0x3f));
          CHECK_EQ((u3 & 0x3f), ((c >> 6) & 0x3f));
          CHECK_EQ((u2 & 0x3f), ((c >> 12) & 0x3f));
          CHECK_EQ((u1 & 0x3), c >> 18);
        }
      }
    }
  }
}


THREADED_TEST(Utf16) {
  LocalContext context;
  v8::HandleScope scope(context->GetIsolate());
  CompileRun(
      "var pad = '01234567890123456789';"
      "var p = [];"
      "var plens = [20, 3, 3];"
      "p.push('01234567890123456789');"
      "var lead = 0xd800;"
      "var trail = 0xdc00;"
      "p.push(String.fromCharCode(0xd800));"
      "p.push(String.fromCharCode(0xdc00));"
      "var a = [];"
      "var b = [];"
      "var c = [];"
      "var alens = [];"
      "for (var i = 0; i < 3; i++) {"
      "  p[1] = String.fromCharCode(lead++);"
      "  for (var j = 0; j < 3; j++) {"
      "    p[2] = String.fromCharCode(trail++);"
      "    a.push(p[i] + p[j]);"
      "    b.push(p[i] + p[j]);"
      "    c.push(p[i] + p[j]);"
      "    alens.push(plens[i] + plens[j]);"
      "  }"
      "}"
      "alens[5] -= 2;"  // Here the surrogate pairs match up.
      "var a2 = [];"
      "var b2 = [];"
      "var c2 = [];"
      "var a2lens = [];"
      "for (var m = 0; m < 9; m++) {"
      "  for (var n = 0; n < 9; n++) {"
      "    a2.push(a[m] + a[n]);"
      "    b2.push(b[m] + b[n]);"
      "    var newc = 'x' + c[m] + c[n] + 'y';"
      "    c2.push(newc.substring(1, newc.length - 1));"
      "    var utf = alens[m] + alens[n];"  // And here.
           // The 'n's that start with 0xdc.. are 6-8
           // The 'm's that end with 0xd8.. are 1, 4 and 7
      "    if ((m % 3) == 1 && n >= 6) utf -= 2;"
      "    a2lens.push(utf);"
      "  }"
      "}");
  Utf16Helper(context, "a", "alens", 9);
  Utf16Helper(context, "a2", "a2lens", 81);
  WriteUtf8Helper(context, "b", "alens", 9);
  WriteUtf8Helper(context, "b2", "a2lens", 81);
  WriteUtf8Helper(context, "c2", "a2lens", 81);
}


static bool SameSymbol(Handle<String> s1, Handle<String> s2) {
  i::Handle<i::String> is1(v8::Utils::OpenHandle(*s1));
  i::Handle<i::String> is2(v8::Utils::OpenHandle(*s2));
  return *is1 == *is2;
}

static void SameSymbolHelper(v8::Isolate* isolate, const char* a,
                             const char* b) {
  Handle<String> symbol1 =
      v8::String::NewFromUtf8(isolate, a, v8::String::kInternalizedString);
  Handle<String> symbol2 =
      v8::String::NewFromUtf8(isolate, b, v8::String::kInternalizedString);
  CHECK(SameSymbol(symbol1, symbol2));
}


THREADED_TEST(Utf16Symbol) {
  LocalContext context;
  v8::HandleScope scope(context->GetIsolate());

  Handle<String> symbol1 = v8::String::NewFromUtf8(
      context->GetIsolate(), "abc", v8::String::kInternalizedString);
  Handle<String> symbol2 = v8::String::NewFromUtf8(
      context->GetIsolate(), "abc", v8::String::kInternalizedString);
  CHECK(SameSymbol(symbol1, symbol2));

  SameSymbolHelper(context->GetIsolate(),
                   "\360\220\220\205",  // 4 byte encoding.
                   "\355\240\201\355\260\205");  // 2 3-byte surrogates.
  SameSymbolHelper(context->GetIsolate(),
                   "\355\240\201\355\260\206",  // 2 3-byte surrogates.
                   "\360\220\220\206");  // 4 byte encoding.
  SameSymbolHelper(context->GetIsolate(),
                   "x\360\220\220\205",  // 4 byte encoding.
                   "x\355\240\201\355\260\205");  // 2 3-byte surrogates.
  SameSymbolHelper(context->GetIsolate(),
                   "x\355\240\201\355\260\206",  // 2 3-byte surrogates.
                   "x\360\220\220\206");  // 4 byte encoding.
  CompileRun(
      "var sym0 = 'benedictus';"
      "var sym0b = 'S\303\270ren';"
      "var sym1 = '\355\240\201\355\260\207';"
      "var sym2 = '\360\220\220\210';"
      "var sym3 = 'x\355\240\201\355\260\207';"
      "var sym4 = 'x\360\220\220\210';"
      "if (sym1.length != 2) throw sym1;"
      "if (sym1.charCodeAt(1) != 0xdc07) throw sym1.charCodeAt(1);"
      "if (sym2.length != 2) throw sym2;"
      "if (sym2.charCodeAt(1) != 0xdc08) throw sym2.charCodeAt(2);"
      "if (sym3.length != 3) throw sym3;"
      "if (sym3.charCodeAt(2) != 0xdc07) throw sym1.charCodeAt(2);"
      "if (sym4.length != 3) throw sym4;"
      "if (sym4.charCodeAt(2) != 0xdc08) throw sym2.charCodeAt(2);");
  Handle<String> sym0 = v8::String::NewFromUtf8(
      context->GetIsolate(), "benedictus", v8::String::kInternalizedString);
  Handle<String> sym0b = v8::String::NewFromUtf8(
      context->GetIsolate(), "S\303\270ren", v8::String::kInternalizedString);
  Handle<String> sym1 =
      v8::String::NewFromUtf8(context->GetIsolate(), "\355\240\201\355\260\207",
                              v8::String::kInternalizedString);
  Handle<String> sym2 =
      v8::String::NewFromUtf8(context->GetIsolate(), "\360\220\220\210",
                              v8::String::kInternalizedString);
  Handle<String> sym3 = v8::String::NewFromUtf8(
      context->GetIsolate(), "x\355\240\201\355\260\207",
      v8::String::kInternalizedString);
  Handle<String> sym4 =
      v8::String::NewFromUtf8(context->GetIsolate(), "x\360\220\220\210",
                              v8::String::kInternalizedString);
  v8::Local<v8::Object> global = context->Global();
  Local<Value> s0 = global->Get(v8_str("sym0"));
  Local<Value> s0b = global->Get(v8_str("sym0b"));
  Local<Value> s1 = global->Get(v8_str("sym1"));
  Local<Value> s2 = global->Get(v8_str("sym2"));
  Local<Value> s3 = global->Get(v8_str("sym3"));
  Local<Value> s4 = global->Get(v8_str("sym4"));
  CHECK(SameSymbol(sym0, Handle<String>::Cast(s0)));
  CHECK(SameSymbol(sym0b, Handle<String>::Cast(s0b)));
  CHECK(SameSymbol(sym1, Handle<String>::Cast(s1)));
  CHECK(SameSymbol(sym2, Handle<String>::Cast(s2)));
  CHECK(SameSymbol(sym3, Handle<String>::Cast(s3)));
  CHECK(SameSymbol(sym4, Handle<String>::Cast(s4)));
}


THREADED_TEST(ToArrayIndex) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);

  v8::Handle<String> str = v8_str("42");
  v8::Handle<v8::Uint32> index = str->ToArrayIndex();
  CHECK(!index.IsEmpty());
  CHECK_EQ(42.0, index->Uint32Value());
  str = v8_str("42asdf");
  index = str->ToArrayIndex();
  CHECK(index.IsEmpty());
  str = v8_str("-42");
  index = str->ToArrayIndex();
  CHECK(index.IsEmpty());
  str = v8_str("4294967295");
  index = str->ToArrayIndex();
  CHECK(!index.IsEmpty());
  CHECK_EQ(4294967295.0, index->Uint32Value());
  v8::Handle<v8::Number> num = v8::Number::New(isolate, 1);
  index = num->ToArrayIndex();
  CHECK(!index.IsEmpty());
  CHECK_EQ(1.0, index->Uint32Value());
  num = v8::Number::New(isolate, -1);
  index = num->ToArrayIndex();
  CHECK(index.IsEmpty());
  v8::Handle<v8::Object> obj = v8::Object::New(isolate);
  index = obj->ToArrayIndex();
  CHECK(index.IsEmpty());
}


THREADED_TEST(ErrorConstruction) {
  LocalContext context;
  v8::HandleScope scope(context->GetIsolate());

  v8::Handle<String> foo = v8_str("foo");
  v8::Handle<String> message = v8_str("message");
  v8::Handle<Value> range_error = v8::Exception::RangeError(foo);
  CHECK(range_error->IsObject());
  CHECK(range_error.As<v8::Object>()->Get(message)->Equals(foo));
  v8::Handle<Value> reference_error = v8::Exception::ReferenceError(foo);
  CHECK(reference_error->IsObject());
  CHECK(reference_error.As<v8::Object>()->Get(message)->Equals(foo));
  v8::Handle<Value> syntax_error = v8::Exception::SyntaxError(foo);
  CHECK(syntax_error->IsObject());
  CHECK(syntax_error.As<v8::Object>()->Get(message)->Equals(foo));
  v8::Handle<Value> type_error = v8::Exception::TypeError(foo);
  CHECK(type_error->IsObject());
  CHECK(type_error.As<v8::Object>()->Get(message)->Equals(foo));
  v8::Handle<Value> error = v8::Exception::Error(foo);
  CHECK(error->IsObject());
  CHECK(error.As<v8::Object>()->Get(message)->Equals(foo));
}


static void ThrowV8Exception(const v8::FunctionCallbackInfo<v8::Value>& info) {
  ApiTestFuzzer::Fuzz();
  v8::Handle<String> foo = v8_str("foo");
  v8::Handle<String> message = v8_str("message");
  v8::Handle<Value> error = v8::Exception::Error(foo);
  CHECK(error->IsObject());
  CHECK(error.As<v8::Object>()->Get(message)->Equals(foo));
  info.GetIsolate()->ThrowException(error);
  info.GetReturnValue().SetUndefined();
}


THREADED_TEST(ExceptionCreateMessage) {
  LocalContext context;
  v8::HandleScope scope(context->GetIsolate());
  v8::Handle<String> foo_str = v8_str("foo");
  v8::Handle<String> message_str = v8_str("message");

  v8::V8::SetCaptureStackTraceForUncaughtExceptions(true);

  Local<v8::FunctionTemplate> fun =
      v8::FunctionTemplate::New(context->GetIsolate(), ThrowV8Exception);
  v8::Local<v8::Object> global = context->Global();
  global->Set(v8_str("throwV8Exception"), fun->GetFunction());

  TryCatch try_catch;
  CompileRun(
      "function f1() {\n"
      "  throwV8Exception();\n"
      "};\n"
      "f1();");
  CHECK(try_catch.HasCaught());

  v8::Handle<v8::Value> error = try_catch.Exception();
  CHECK(error->IsObject());
  CHECK(error.As<v8::Object>()->Get(message_str)->Equals(foo_str));

  v8::Handle<v8::Message> message = v8::Exception::CreateMessage(error);
  CHECK(!message.IsEmpty());
  CHECK_EQ(2, message->GetLineNumber());
  CHECK_EQ(2, message->GetStartColumn());

  v8::Handle<v8::StackTrace> stackTrace = message->GetStackTrace();
  CHECK(!stackTrace.IsEmpty());
  CHECK_EQ(2, stackTrace->GetFrameCount());

  stackTrace = v8::Exception::GetStackTrace(error);
  CHECK(!stackTrace.IsEmpty());
  CHECK_EQ(2, stackTrace->GetFrameCount());

  v8::V8::SetCaptureStackTraceForUncaughtExceptions(false);

  // Now check message location when SetCaptureStackTraceForUncaughtExceptions
  // is false.
  try_catch.Reset();

  CompileRun(
      "function f2() {\n"
      "  return throwV8Exception();\n"
      "};\n"
      "f2();");
  CHECK(try_catch.HasCaught());

  error = try_catch.Exception();
  CHECK(error->IsObject());
  CHECK(error.As<v8::Object>()->Get(message_str)->Equals(foo_str));

  message = v8::Exception::CreateMessage(error);
  CHECK(!message.IsEmpty());
  CHECK_EQ(2, message->GetLineNumber());
  CHECK_EQ(9, message->GetStartColumn());

  // Should be empty stack trace.
  stackTrace = message->GetStackTrace();
  CHECK(stackTrace.IsEmpty());
  CHECK(v8::Exception::GetStackTrace(error).IsEmpty());
}


static void YGetter(Local<String> name,
                    const v8::PropertyCallbackInfo<v8::Value>& info) {
  ApiTestFuzzer::Fuzz();
  info.GetReturnValue().Set(v8_num(10));
}


static void YSetter(Local<String> name,
                    Local<Value> value,
                    const v8::PropertyCallbackInfo<void>& info) {
  Local<Object> this_obj = Local<Object>::Cast(info.This());
  if (this_obj->Has(name)) this_obj->Delete(name);
  this_obj->Set(name, value);
}


THREADED_TEST(DeleteAccessor) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  v8::Handle<v8::ObjectTemplate> obj = ObjectTemplate::New(isolate);
  obj->SetAccessor(v8_str("y"), YGetter, YSetter);
  LocalContext context;
  v8::Handle<v8::Object> holder = obj->NewInstance();
  context->Global()->Set(v8_str("holder"), holder);
  v8::Handle<Value> result = CompileRun(
      "holder.y = 11; holder.y = 12; holder.y");
  CHECK_EQ(12u, result->Uint32Value());
}


THREADED_TEST(TypeSwitch) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  v8::Handle<v8::FunctionTemplate> templ1 = v8::FunctionTemplate::New(isolate);
  v8::Handle<v8::FunctionTemplate> templ2 = v8::FunctionTemplate::New(isolate);
  v8::Handle<v8::FunctionTemplate> templ3 = v8::FunctionTemplate::New(isolate);
  v8::Handle<v8::FunctionTemplate> templs[3] = { templ1, templ2, templ3 };
  v8::Handle<v8::TypeSwitch> type_switch = v8::TypeSwitch::New(3, templs);
  LocalContext context;
  v8::Handle<v8::Object> obj0 = v8::Object::New(isolate);
  v8::Handle<v8::Object> obj1 = templ1->GetFunction()->NewInstance();
  v8::Handle<v8::Object> obj2 = templ2->GetFunction()->NewInstance();
  v8::Handle<v8::Object> obj3 = templ3->GetFunction()->NewInstance();
  for (int i = 0; i < 10; i++) {
    CHECK_EQ(0, type_switch->match(obj0));
    CHECK_EQ(1, type_switch->match(obj1));
    CHECK_EQ(2, type_switch->match(obj2));
    CHECK_EQ(3, type_switch->match(obj3));
    CHECK_EQ(3, type_switch->match(obj3));
    CHECK_EQ(2, type_switch->match(obj2));
    CHECK_EQ(1, type_switch->match(obj1));
    CHECK_EQ(0, type_switch->match(obj0));
  }
}


static int trouble_nesting = 0;
static void TroubleCallback(const v8::FunctionCallbackInfo<v8::Value>& args) {
  ApiTestFuzzer::Fuzz();
  trouble_nesting++;

  // Call a JS function that throws an uncaught exception.
  Local<v8::Object> arg_this =
      args.GetIsolate()->GetCurrentContext()->Global();
  Local<Value> trouble_callee = (trouble_nesting == 3) ?
    arg_this->Get(v8_str("trouble_callee")) :
    arg_this->Get(v8_str("trouble_caller"));
  CHECK(trouble_callee->IsFunction());
  args.GetReturnValue().Set(
      Function::Cast(*trouble_callee)->Call(arg_this, 0, NULL));
}


static int report_count = 0;
static void ApiUncaughtExceptionTestListener(v8::Handle<v8::Message>,
                                             v8::Handle<Value>) {
  report_count++;
}


// Counts uncaught exceptions, but other tests running in parallel
// also have uncaught exceptions.
TEST(ApiUncaughtException) {
  report_count = 0;
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);
  v8::V8::AddMessageListener(ApiUncaughtExceptionTestListener);

  Local<v8::FunctionTemplate> fun =
      v8::FunctionTemplate::New(isolate, TroubleCallback);
  v8::Local<v8::Object> global = env->Global();
  global->Set(v8_str("trouble"), fun->GetFunction());

  CompileRun(
      "function trouble_callee() {"
      "  var x = null;"
      "  return x.foo;"
      "};"
      "function trouble_caller() {"
      "  trouble();"
      "};");
  Local<Value> trouble = global->Get(v8_str("trouble"));
  CHECK(trouble->IsFunction());
  Local<Value> trouble_callee = global->Get(v8_str("trouble_callee"));
  CHECK(trouble_callee->IsFunction());
  Local<Value> trouble_caller = global->Get(v8_str("trouble_caller"));
  CHECK(trouble_caller->IsFunction());
  Function::Cast(*trouble_caller)->Call(global, 0, NULL);
  CHECK_EQ(1, report_count);
  v8::V8::RemoveMessageListeners(ApiUncaughtExceptionTestListener);
}


TEST(ApiUncaughtExceptionInObjectObserve) {
  v8::internal::FLAG_stack_size = 150;
  report_count = 0;
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);
  v8::V8::AddMessageListener(ApiUncaughtExceptionTestListener);
  CompileRun(
      "var obj = {};"
      "var observe_count = 0;"
      "function observer1() { ++observe_count; };"
      "function observer2() { ++observe_count; };"
      "function observer_throws() { throw new Error(); };"
      "function stack_overflow() { return (function f(x) { f(x+1); })(0); };"
      "Object.observe(obj, observer_throws.bind());"
      "Object.observe(obj, observer1);"
      "Object.observe(obj, stack_overflow);"
      "Object.observe(obj, observer2);"
      "Object.observe(obj, observer_throws.bind());"
      "obj.foo = 'bar';");
  CHECK_EQ(3, report_count);
  ExpectInt32("observe_count", 2);
  v8::V8::RemoveMessageListeners(ApiUncaughtExceptionTestListener);
}


static const char* script_resource_name = "ExceptionInNativeScript.js";
static void ExceptionInNativeScriptTestListener(v8::Handle<v8::Message> message,
                                                v8::Handle<Value>) {
  v8::Handle<v8::Value> name_val = message->GetScriptOrigin().ResourceName();
  CHECK(!name_val.IsEmpty() && name_val->IsString());
  v8::String::Utf8Value name(message->GetScriptOrigin().ResourceName());
  CHECK_EQ(0, strcmp(script_resource_name, *name));
  CHECK_EQ(3, message->GetLineNumber());
  v8::String::Utf8Value source_line(message->GetSourceLine());
  CHECK_EQ(0, strcmp("  new o.foo();", *source_line));
}


TEST(ExceptionInNativeScript) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);
  v8::V8::AddMessageListener(ExceptionInNativeScriptTestListener);

  Local<v8::FunctionTemplate> fun =
      v8::FunctionTemplate::New(isolate, TroubleCallback);
  v8::Local<v8::Object> global = env->Global();
  global->Set(v8_str("trouble"), fun->GetFunction());

  CompileRunWithOrigin(
      "function trouble() {\n"
      "  var o = {};\n"
      "  new o.foo();\n"
      "};",
      script_resource_name);
  Local<Value> trouble = global->Get(v8_str("trouble"));
  CHECK(trouble->IsFunction());
  Function::Cast(*trouble)->Call(global, 0, NULL);
  v8::V8::RemoveMessageListeners(ExceptionInNativeScriptTestListener);
}


TEST(CompilationErrorUsingTryCatchHandler) {
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());
  v8::TryCatch try_catch;
  v8_compile("This doesn't &*&@#$&*^ compile.");
  CHECK(*try_catch.Exception());
  CHECK(try_catch.HasCaught());
}


TEST(TryCatchFinallyUsingTryCatchHandler) {
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());
  v8::TryCatch try_catch;
  CompileRun("try { throw ''; } catch (e) {}");
  CHECK(!try_catch.HasCaught());
  CompileRun("try { throw ''; } finally {}");
  CHECK(try_catch.HasCaught());
  try_catch.Reset();
  CompileRun(
      "(function() {"
      "try { throw ''; } finally { return; }"
      "})()");
  CHECK(!try_catch.HasCaught());
  CompileRun(
      "(function()"
      "  { try { throw ''; } finally { throw 0; }"
      "})()");
  CHECK(try_catch.HasCaught());
}


void CEvaluate(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::HandleScope scope(args.GetIsolate());
  CompileRun(args[0]->ToString(args.GetIsolate()));
}


TEST(TryCatchFinallyStoresMessageUsingTryCatchHandler) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  Local<ObjectTemplate> templ = ObjectTemplate::New(isolate);
  templ->Set(v8_str("CEvaluate"),
             v8::FunctionTemplate::New(isolate, CEvaluate));
  LocalContext context(0, templ);
  v8::TryCatch try_catch;
  CompileRun("try {"
             "  CEvaluate('throw 1;');"
             "} finally {"
             "}");
  CHECK(try_catch.HasCaught());
  CHECK(!try_catch.Message().IsEmpty());
  String::Utf8Value exception_value(try_catch.Exception());
  CHECK_EQ(0, strcmp(*exception_value, "1"));
  try_catch.Reset();
  CompileRun("try {"
             "  CEvaluate('throw 1;');"
             "} finally {"
             "  throw 2;"
             "}");
  CHECK(try_catch.HasCaught());
  CHECK(!try_catch.Message().IsEmpty());
  String::Utf8Value finally_exception_value(try_catch.Exception());
  CHECK_EQ(0, strcmp(*finally_exception_value, "2"));
}


// For use within the TestSecurityHandler() test.
static bool g_security_callback_result = false;
static bool SecurityTestCallback(Local<v8::Object> global, Local<Value> name,
                                 v8::AccessType type, Local<Value> data) {
  printf("a\n");
  return g_security_callback_result;
}


// SecurityHandler can't be run twice
TEST(SecurityHandler) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope0(isolate);
  v8::Handle<v8::ObjectTemplate> global_template =
      v8::ObjectTemplate::New(isolate);
  global_template->SetAccessCheckCallbacks(SecurityTestCallback, NULL);
  // Create an environment
  v8::Handle<Context> context0 = Context::New(isolate, NULL, global_template);
  context0->Enter();

  v8::Handle<v8::Object> global0 = context0->Global();
  v8::Handle<Script> script0 = v8_compile("foo = 111");
  script0->Run();
  global0->Set(v8_str("0"), v8_num(999));
  v8::Handle<Value> foo0 = global0->Get(v8_str("foo"));
  CHECK_EQ(111, foo0->Int32Value());
  v8::Handle<Value> z0 = global0->Get(v8_str("0"));
  CHECK_EQ(999, z0->Int32Value());

  // Create another environment, should fail security checks.
  v8::HandleScope scope1(isolate);

  v8::Handle<Context> context1 =
    Context::New(isolate, NULL, global_template);
  context1->Enter();

  v8::Handle<v8::Object> global1 = context1->Global();
  global1->Set(v8_str("othercontext"), global0);
  // This set will fail the security check.
  v8::Handle<Script> script1 =
    v8_compile("othercontext.foo = 222; othercontext[0] = 888;");
  script1->Run();
  g_security_callback_result = true;
  // This read will pass the security check.
  v8::Handle<Value> foo1 = global0->Get(v8_str("foo"));
  CHECK_EQ(111, foo1->Int32Value());
  // This read will pass the security check.
  v8::Handle<Value> z1 = global0->Get(v8_str("0"));
  CHECK_EQ(999, z1->Int32Value());

  // Create another environment, should pass security checks.
  {
    v8::HandleScope scope2(isolate);
    LocalContext context2;
    v8::Handle<v8::Object> global2 = context2->Global();
    global2->Set(v8_str("othercontext"), global0);
    v8::Handle<Script> script2 =
        v8_compile("othercontext.foo = 333; othercontext[0] = 888;");
    script2->Run();
    v8::Handle<Value> foo2 = global0->Get(v8_str("foo"));
    CHECK_EQ(333, foo2->Int32Value());
    v8::Handle<Value> z2 = global0->Get(v8_str("0"));
    CHECK_EQ(888, z2->Int32Value());
  }

  context1->Exit();
  context0->Exit();
}


THREADED_TEST(SecurityChecks) {
  LocalContext env1;
  v8::HandleScope handle_scope(env1->GetIsolate());
  v8::Handle<Context> env2 = Context::New(env1->GetIsolate());

  Local<Value> foo = v8_str("foo");
  Local<Value> bar = v8_str("bar");

  // Set to the same domain.
  env1->SetSecurityToken(foo);

  // Create a function in env1.
  CompileRun("spy=function(){return spy;}");
  Local<Value> spy = env1->Global()->Get(v8_str("spy"));
  CHECK(spy->IsFunction());

  // Create another function accessing global objects.
  CompileRun("spy2=function(){return new this.Array();}");
  Local<Value> spy2 = env1->Global()->Get(v8_str("spy2"));
  CHECK(spy2->IsFunction());

  // Switch to env2 in the same domain and invoke spy on env2.
  {
    env2->SetSecurityToken(foo);
    // Enter env2
    Context::Scope scope_env2(env2);
    Local<Value> result = Function::Cast(*spy)->Call(env2->Global(), 0, NULL);
    CHECK(result->IsFunction());
  }

  {
    env2->SetSecurityToken(bar);
    Context::Scope scope_env2(env2);

    // Call cross_domain_call, it should throw an exception
    v8::TryCatch try_catch;
    Function::Cast(*spy2)->Call(env2->Global(), 0, NULL);
    CHECK(try_catch.HasCaught());
  }
}


// Regression test case for issue 1183439.
THREADED_TEST(SecurityChecksForPrototypeChain) {
  LocalContext current;
  v8::HandleScope scope(current->GetIsolate());
  v8::Handle<Context> other = Context::New(current->GetIsolate());

  // Change context to be able to get to the Object function in the
  // other context without hitting the security checks.
  v8::Local<Value> other_object;
  { Context::Scope scope(other);
    other_object = other->Global()->Get(v8_str("Object"));
    other->Global()->Set(v8_num(42), v8_num(87));
  }

  current->Global()->Set(v8_str("other"), other->Global());
  CHECK(v8_compile("other")->Run()->Equals(other->Global()));

  // Make sure the security check fails here and we get an undefined
  // result instead of getting the Object function. Repeat in a loop
  // to make sure to exercise the IC code.
  v8::Local<Script> access_other0 = v8_compile("other.Object");
  v8::Local<Script> access_other1 = v8_compile("other[42]");
  for (int i = 0; i < 5; i++) {
    CHECK(access_other0->Run().IsEmpty());
    CHECK(access_other1->Run().IsEmpty());
  }

  // Create an object that has 'other' in its prototype chain and make
  // sure we cannot access the Object function indirectly through
  // that. Repeat in a loop to make sure to exercise the IC code.
  v8_compile("function F() { };"
             "F.prototype = other;"
             "var f = new F();")->Run();
  v8::Local<Script> access_f0 = v8_compile("f.Object");
  v8::Local<Script> access_f1 = v8_compile("f[42]");
  for (int j = 0; j < 5; j++) {
    CHECK(access_f0->Run().IsEmpty());
    CHECK(access_f1->Run().IsEmpty());
  }

  // Now it gets hairy: Set the prototype for the other global object
  // to be the current global object. The prototype chain for 'f' now
  // goes through 'other' but ends up in the current global object.
  { Context::Scope scope(other);
    other->Global()->Set(v8_str("__proto__"), current->Global());
  }
  // Set a named and an index property on the current global
  // object. To force the lookup to go through the other global object,
  // the properties must not exist in the other global object.
  current->Global()->Set(v8_str("foo"), v8_num(100));
  current->Global()->Set(v8_num(99), v8_num(101));
  // Try to read the properties from f and make sure that the access
  // gets stopped by the security checks on the other global object.
  Local<Script> access_f2 = v8_compile("f.foo");
  Local<Script> access_f3 = v8_compile("f[99]");
  for (int k = 0; k < 5; k++) {
    CHECK(access_f2->Run().IsEmpty());
    CHECK(access_f3->Run().IsEmpty());
  }
}


static bool security_check_with_gc_called;

static bool SecurityTestCallbackWithGC(Local<v8::Object> global,
                                       Local<v8::Value> name,
                                       v8::AccessType type, Local<Value> data) {
  CcTest::heap()->CollectAllGarbage(i::Heap::kNoGCFlags);
  security_check_with_gc_called = true;
  return true;
}


TEST(SecurityTestGCAllowed) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope handle_scope(isolate);
  v8::Handle<v8::ObjectTemplate> object_template =
      v8::ObjectTemplate::New(isolate);
  object_template->SetAccessCheckCallbacks(SecurityTestCallbackWithGC, NULL);

  v8::Handle<Context> context = Context::New(isolate);
  v8::Context::Scope context_scope(context);

  context->Global()->Set(v8_str("obj"), object_template->NewInstance());

  security_check_with_gc_called = false;
  CompileRun("obj[0] = new String(1002);");
  CHECK(security_check_with_gc_called);

  security_check_with_gc_called = false;
  CHECK(CompileRun("obj[0]")->ToString(isolate)->Equals(v8_str("1002")));
  CHECK(security_check_with_gc_called);
}


THREADED_TEST(CrossDomainDelete) {
  LocalContext env1;
  v8::HandleScope handle_scope(env1->GetIsolate());
  v8::Handle<Context> env2 = Context::New(env1->GetIsolate());

  Local<Value> foo = v8_str("foo");
  Local<Value> bar = v8_str("bar");

  // Set to the same domain.
  env1->SetSecurityToken(foo);
  env2->SetSecurityToken(foo);

  env1->Global()->Set(v8_str("prop"), v8_num(3));
  env2->Global()->Set(v8_str("env1"), env1->Global());

  // Change env2 to a different domain and delete env1.prop.
  env2->SetSecurityToken(bar);
  {
    Context::Scope scope_env2(env2);
    Local<Value> result =
        CompileRun("delete env1.prop");
    CHECK(result.IsEmpty());
  }

  // Check that env1.prop still exists.
  Local<Value> v = env1->Global()->Get(v8_str("prop"));
  CHECK(v->IsNumber());
  CHECK_EQ(3, v->Int32Value());
}


THREADED_TEST(CrossDomainIsPropertyEnumerable) {
  LocalContext env1;
  v8::HandleScope handle_scope(env1->GetIsolate());
  v8::Handle<Context> env2 = Context::New(env1->GetIsolate());

  Local<Value> foo = v8_str("foo");
  Local<Value> bar = v8_str("bar");

  // Set to the same domain.
  env1->SetSecurityToken(foo);
  env2->SetSecurityToken(foo);

  env1->Global()->Set(v8_str("prop"), v8_num(3));
  env2->Global()->Set(v8_str("env1"), env1->Global());

  // env1.prop is enumerable in env2.
  Local<String> test = v8_str("propertyIsEnumerable.call(env1, 'prop')");
  {
    Context::Scope scope_env2(env2);
    Local<Value> result = CompileRun(test);
    CHECK(result->IsTrue());
  }

  // Change env2 to a different domain and test again.
  env2->SetSecurityToken(bar);
  {
    Context::Scope scope_env2(env2);
    Local<Value> result = CompileRun(test);
    CHECK(result.IsEmpty());
  }
}


THREADED_TEST(CrossDomainForIn) {
  LocalContext env1;
  v8::HandleScope handle_scope(env1->GetIsolate());
  v8::Handle<Context> env2 = Context::New(env1->GetIsolate());

  Local<Value> foo = v8_str("foo");
  Local<Value> bar = v8_str("bar");

  // Set to the same domain.
  env1->SetSecurityToken(foo);
  env2->SetSecurityToken(foo);

  env1->Global()->Set(v8_str("prop"), v8_num(3));
  env2->Global()->Set(v8_str("env1"), env1->Global());

  // Change env2 to a different domain and set env1's global object
  // as the __proto__ of an object in env2 and enumerate properties
  // in for-in. It shouldn't enumerate properties on env1's global
  // object.
  env2->SetSecurityToken(bar);
  {
    Context::Scope scope_env2(env2);
    Local<Value> result = CompileRun(
        "(function() {"
        "  var obj = { '__proto__': env1 };"
        "  try {"
        "    for (var p in obj) {"
        "      if (p == 'prop') return false;"
        "    }"
        "    return false;"
        "  } catch (e) {"
        "    return true;"
        "  }"
        "})()");
    CHECK(result->IsTrue());
  }
}


TEST(ContextDetachGlobal) {
  LocalContext env1;
  v8::HandleScope handle_scope(env1->GetIsolate());
  v8::Handle<Context> env2 = Context::New(env1->GetIsolate());

  Local<v8::Object> global1 = env1->Global();

  Local<Value> foo = v8_str("foo");

  // Set to the same domain.
  env1->SetSecurityToken(foo);
  env2->SetSecurityToken(foo);

  // Enter env2
  env2->Enter();

  // Create a function in env2 and add a reference to it in env1.
  Local<v8::Object> global2 = env2->Global();
  global2->Set(v8_str("prop"), v8::Integer::New(env2->GetIsolate(), 1));
  CompileRun("function getProp() {return prop;}");

  env1->Global()->Set(v8_str("getProp"),
                      global2->Get(v8_str("getProp")));

  // Detach env2's global, and reuse the global object of env2
  env2->Exit();
  env2->DetachGlobal();

  v8::Handle<Context> env3 = Context::New(env1->GetIsolate(),
                                          0,
                                          v8::Handle<v8::ObjectTemplate>(),
                                          global2);
  env3->SetSecurityToken(v8_str("bar"));
  env3->Enter();

  Local<v8::Object> global3 = env3->Global();
  CHECK(global2->Equals(global3));
  CHECK(global3->Get(v8_str("prop"))->IsUndefined());
  CHECK(global3->Get(v8_str("getProp"))->IsUndefined());
  global3->Set(v8_str("prop"), v8::Integer::New(env3->GetIsolate(), -1));
  global3->Set(v8_str("prop2"), v8::Integer::New(env3->GetIsolate(), 2));
  env3->Exit();

  // Call getProp in env1, and it should return the value 1
  {
    Local<Value> get_prop = global1->Get(v8_str("getProp"));
    CHECK(get_prop->IsFunction());
    v8::TryCatch try_catch;
    Local<Value> r = Function::Cast(*get_prop)->Call(global1, 0, NULL);
    CHECK(!try_catch.HasCaught());
    CHECK_EQ(1, r->Int32Value());
  }

  // Check that env3 is not accessible from env1
  {
    Local<Value> r = global3->Get(v8_str("prop2"));
    CHECK(r.IsEmpty());
  }
}


TEST(DetachGlobal) {
  LocalContext env1;
  v8::HandleScope scope(env1->GetIsolate());

  // Create second environment.
  v8::Handle<Context> env2 = Context::New(env1->GetIsolate());

  Local<Value> foo = v8_str("foo");

  // Set same security token for env1 and env2.
  env1->SetSecurityToken(foo);
  env2->SetSecurityToken(foo);

  // Create a property on the global object in env2.
  {
    v8::Context::Scope scope(env2);
    env2->Global()->Set(v8_str("p"), v8::Integer::New(env2->GetIsolate(), 42));
  }

  // Create a reference to env2 global from env1 global.
  env1->Global()->Set(v8_str("other"), env2->Global());

  // Check that we have access to other.p in env2 from env1.
  Local<Value> result = CompileRun("other.p");
  CHECK(result->IsInt32());
  CHECK_EQ(42, result->Int32Value());

  // Hold on to global from env2 and detach global from env2.
  Local<v8::Object> global2 = env2->Global();
  env2->DetachGlobal();

  // Check that the global has been detached. No other.p property can
  // be found.
  result = CompileRun("other.p");
  CHECK(result.IsEmpty());

  // Reuse global2 for env3.
  v8::Handle<Context> env3 = Context::New(env1->GetIsolate(),
                                          0,
                                          v8::Handle<v8::ObjectTemplate>(),
                                          global2);
  CHECK(global2->Equals(env3->Global()));

  // Start by using the same security token for env3 as for env1 and env2.
  env3->SetSecurityToken(foo);

  // Create a property on the global object in env3.
  {
    v8::Context::Scope scope(env3);
    env3->Global()->Set(v8_str("p"), v8::Integer::New(env3->GetIsolate(), 24));
  }

  // Check that other.p is now the property in env3 and that we have access.
  result = CompileRun("other.p");
  CHECK(result->IsInt32());
  CHECK_EQ(24, result->Int32Value());

  // Change security token for env3 to something different from env1 and env2.
  env3->SetSecurityToken(v8_str("bar"));

  // Check that we do not have access to other.p in env1. |other| is now
  // the global object for env3 which has a different security token,
  // so access should be blocked.
  result = CompileRun("other.p");
  CHECK(result.IsEmpty());
}


void GetThisX(const v8::FunctionCallbackInfo<v8::Value>& info) {
  info.GetReturnValue().Set(
      info.GetIsolate()->GetCurrentContext()->Global()->Get(v8_str("x")));
}


TEST(DetachedAccesses) {
  LocalContext env1;
  v8::HandleScope scope(env1->GetIsolate());

  // Create second environment.
  Local<ObjectTemplate> inner_global_template =
      FunctionTemplate::New(env1->GetIsolate())->InstanceTemplate();
  inner_global_template ->SetAccessorProperty(
      v8_str("this_x"), FunctionTemplate::New(env1->GetIsolate(), GetThisX));
  v8::Local<Context> env2 =
      Context::New(env1->GetIsolate(), NULL, inner_global_template);

  Local<Value> foo = v8_str("foo");

  // Set same security token for env1 and env2.
  env1->SetSecurityToken(foo);
  env2->SetSecurityToken(foo);

  env1->Global()->Set(v8_str("x"), v8_str("env1_x"));

  {
    v8::Context::Scope scope(env2);
    env2->Global()->Set(v8_str("x"), v8_str("env2_x"));
    CompileRun(
        "function bound_x() { return x; }"
        "function get_x()   { return this.x; }"
        "function get_x_w() { return (function() {return this.x;})(); }");
    env1->Global()->Set(v8_str("bound_x"), CompileRun("bound_x"));
    env1->Global()->Set(v8_str("get_x"), CompileRun("get_x"));
    env1->Global()->Set(v8_str("get_x_w"), CompileRun("get_x_w"));
    env1->Global()->Set(
        v8_str("this_x"),
        CompileRun("Object.getOwnPropertyDescriptor(this, 'this_x').get"));
  }

  Local<Object> env2_global = env2->Global();
  env2_global->TurnOnAccessCheck();
  env2->DetachGlobal();

  Local<Value> result;
  result = CompileRun("bound_x()");
  CHECK(v8_str("env2_x")->Equals(result));
  result = CompileRun("get_x()");
  CHECK(result.IsEmpty());
  result = CompileRun("get_x_w()");
  CHECK(result.IsEmpty());
  result = CompileRun("this_x()");
  CHECK(v8_str("env2_x")->Equals(result));

  // Reattach env2's proxy
  env2 = Context::New(env1->GetIsolate(),
                      0,
                      v8::Handle<v8::ObjectTemplate>(),
                      env2_global);
  env2->SetSecurityToken(foo);
  {
    v8::Context::Scope scope(env2);
    env2->Global()->Set(v8_str("x"), v8_str("env3_x"));
    env2->Global()->Set(v8_str("env1"), env1->Global());
    result = CompileRun(
        "results = [];"
        "for (var i = 0; i < 4; i++ ) {"
        "  results.push(env1.bound_x());"
        "  results.push(env1.get_x());"
        "  results.push(env1.get_x_w());"
        "  results.push(env1.this_x());"
        "}"
        "results");
    Local<v8::Array> results = Local<v8::Array>::Cast(result);
    CHECK_EQ(16u, results->Length());
    for (int i = 0; i < 16; i += 4) {
      CHECK(v8_str("env2_x")->Equals(results->Get(i + 0)));
      CHECK(v8_str("env1_x")->Equals(results->Get(i + 1)));
      CHECK(v8_str("env3_x")->Equals(results->Get(i + 2)));
      CHECK(v8_str("env2_x")->Equals(results->Get(i + 3)));
    }
  }

  result = CompileRun(
      "results = [];"
      "for (var i = 0; i < 4; i++ ) {"
      "  results.push(bound_x());"
      "  results.push(get_x());"
      "  results.push(get_x_w());"
      "  results.push(this_x());"
      "}"
      "results");
  Local<v8::Array> results = Local<v8::Array>::Cast(result);
  CHECK_EQ(16u, results->Length());
  for (int i = 0; i < 16; i += 4) {
    CHECK(v8_str("env2_x")->Equals(results->Get(i + 0)));
    CHECK(v8_str("env3_x")->Equals(results->Get(i + 1)));
    CHECK(v8_str("env3_x")->Equals(results->Get(i + 2)));
    CHECK(v8_str("env2_x")->Equals(results->Get(i + 3)));
  }

  result = CompileRun(
      "results = [];"
      "for (var i = 0; i < 4; i++ ) {"
      "  results.push(this.bound_x());"
      "  results.push(this.get_x());"
      "  results.push(this.get_x_w());"
      "  results.push(this.this_x());"
      "}"
      "results");
  results = Local<v8::Array>::Cast(result);
  CHECK_EQ(16u, results->Length());
  for (int i = 0; i < 16; i += 4) {
    CHECK(v8_str("env2_x")->Equals(results->Get(i + 0)));
    CHECK(v8_str("env1_x")->Equals(results->Get(i + 1)));
    CHECK(v8_str("env3_x")->Equals(results->Get(i + 2)));
    CHECK(v8_str("env2_x")->Equals(results->Get(i + 3)));
  }
}


static bool allowed_access = false;
static bool AccessBlocker(Local<v8::Object> global, Local<Value> name,
                          v8::AccessType type, Local<Value> data) {
  return CcTest::isolate()->GetCurrentContext()->Global()->Equals(global) ||
         allowed_access;
}


static int g_echo_value = -1;


static void EchoGetter(
    Local<String> name,
    const v8::PropertyCallbackInfo<v8::Value>& info) {
  info.GetReturnValue().Set(v8_num(g_echo_value));
}


static void EchoSetter(Local<String> name,
                       Local<Value> value,
                       const v8::PropertyCallbackInfo<void>&) {
  if (value->IsNumber())
    g_echo_value = value->Int32Value();
}


static void UnreachableGetter(
    Local<String> name,
    const v8::PropertyCallbackInfo<v8::Value>& info) {
  CHECK(false);  // This function should not be called..
}


static void UnreachableSetter(Local<String>,
                              Local<Value>,
                              const v8::PropertyCallbackInfo<void>&) {
  CHECK(false);  // This function should nto be called.
}


static void UnreachableFunction(
    const v8::FunctionCallbackInfo<v8::Value>& info) {
  CHECK(false);  // This function should not be called..
}


TEST(AccessControl) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope handle_scope(isolate);
  v8::Handle<v8::ObjectTemplate> global_template =
      v8::ObjectTemplate::New(isolate);

  global_template->SetAccessCheckCallbacks(AccessBlocker, NULL);

  // Add an accessor accessible by cross-domain JS code.
  global_template->SetAccessor(
      v8_str("accessible_prop"),
      EchoGetter, EchoSetter,
      v8::Handle<Value>(),
      v8::AccessControl(v8::ALL_CAN_READ | v8::ALL_CAN_WRITE));


  // Add an accessor that is not accessible by cross-domain JS code.
  global_template->SetAccessor(v8_str("blocked_prop"),
                               UnreachableGetter, UnreachableSetter,
                               v8::Handle<Value>(),
                               v8::DEFAULT);

  global_template->SetAccessorProperty(
      v8_str("blocked_js_prop"),
      v8::FunctionTemplate::New(isolate, UnreachableFunction),
      v8::FunctionTemplate::New(isolate, UnreachableFunction),
      v8::None,
      v8::DEFAULT);

  // Create an environment
  v8::Local<Context> context0 = Context::New(isolate, NULL, global_template);
  context0->Enter();

  v8::Handle<v8::Object> global0 = context0->Global();

  // Define a property with JS getter and setter.
  CompileRun(
      "function getter() { return 'getter'; };\n"
      "function setter() { return 'setter'; }\n"
      "Object.defineProperty(this, 'js_accessor_p', {get:getter, set:setter})");

  Local<Value> getter = global0->Get(v8_str("getter"));
  Local<Value> setter = global0->Get(v8_str("setter"));

  // And define normal element.
  global0->Set(239, v8_str("239"));

  // Define an element with JS getter and setter.
  CompileRun(
      "function el_getter() { return 'el_getter'; };\n"
      "function el_setter() { return 'el_setter'; };\n"
      "Object.defineProperty(this, '42', {get: el_getter, set: el_setter});");

  Local<Value> el_getter = global0->Get(v8_str("el_getter"));
  Local<Value> el_setter = global0->Get(v8_str("el_setter"));

  v8::HandleScope scope1(isolate);

  v8::Local<Context> context1 = Context::New(isolate);
  context1->Enter();

  v8::Handle<v8::Object> global1 = context1->Global();
  global1->Set(v8_str("other"), global0);

  // Access blocked property.
  CompileRun("other.blocked_prop = 1");

  CHECK(CompileRun("other.blocked_prop").IsEmpty());
  CHECK(CompileRun("Object.getOwnPropertyDescriptor(other, 'blocked_prop')")
            .IsEmpty());
  CHECK(
      CompileRun("propertyIsEnumerable.call(other, 'blocked_prop')").IsEmpty());

  // Access blocked element.
  CHECK(CompileRun("other[239] = 1").IsEmpty());

  CHECK(CompileRun("other[239]").IsEmpty());
  CHECK(CompileRun("Object.getOwnPropertyDescriptor(other, '239')").IsEmpty());
  CHECK(CompileRun("propertyIsEnumerable.call(other, '239')").IsEmpty());

  allowed_access = true;
  // Now we can enumerate the property.
  ExpectTrue("propertyIsEnumerable.call(other, '239')");
  allowed_access = false;

  // Access a property with JS accessor.
  CHECK(CompileRun("other.js_accessor_p = 2").IsEmpty());

  CHECK(CompileRun("other.js_accessor_p").IsEmpty());
  CHECK(CompileRun("Object.getOwnPropertyDescriptor(other, 'js_accessor_p')")
            .IsEmpty());

  allowed_access = true;

  ExpectString("other.js_accessor_p", "getter");
  ExpectObject(
      "Object.getOwnPropertyDescriptor(other, 'js_accessor_p').get", getter);
  ExpectObject(
      "Object.getOwnPropertyDescriptor(other, 'js_accessor_p').set", setter);
  ExpectUndefined(
      "Object.getOwnPropertyDescriptor(other, 'js_accessor_p').value");

  allowed_access = false;

  // Access an element with JS accessor.
  CHECK(CompileRun("other[42] = 2").IsEmpty());

  CHECK(CompileRun("other[42]").IsEmpty());
  CHECK(CompileRun("Object.getOwnPropertyDescriptor(other, '42')").IsEmpty());

  allowed_access = true;

  ExpectString("other[42]", "el_getter");
  ExpectObject("Object.getOwnPropertyDescriptor(other, '42').get", el_getter);
  ExpectObject("Object.getOwnPropertyDescriptor(other, '42').set", el_setter);
  ExpectUndefined("Object.getOwnPropertyDescriptor(other, '42').value");

  allowed_access = false;

  v8::Handle<Value> value;

  // Access accessible property
  value = CompileRun("other.accessible_prop = 3");
  CHECK(value->IsNumber());
  CHECK_EQ(3, value->Int32Value());
  CHECK_EQ(3, g_echo_value);

  value = CompileRun("other.accessible_prop");
  CHECK(value->IsNumber());
  CHECK_EQ(3, value->Int32Value());

  value = CompileRun(
      "Object.getOwnPropertyDescriptor(other, 'accessible_prop').value");
  CHECK(value->IsNumber());
  CHECK_EQ(3, value->Int32Value());

  value = CompileRun("propertyIsEnumerable.call(other, 'accessible_prop')");
  CHECK(value->IsTrue());

  // Enumeration doesn't enumerate accessors from inaccessible objects in
  // the prototype chain even if the accessors are in themselves accessible.
  value = CompileRun(
      "(function() {"
      "  var obj = { '__proto__': other };"
      "  try {"
      "    for (var p in obj) {"
      "      if (p == 'accessible_prop' ||"
      "          p == 'blocked_js_prop' ||"
      "          p == 'blocked_js_prop') {"
      "        return false;"
      "      }"
      "    }"
      "    return false;"
      "  } catch (e) {"
      "    return true;"
      "  }"
      "})()");
  CHECK(value->IsTrue());

  context1->Exit();
  context0->Exit();
}


TEST(AccessControlES5) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope handle_scope(isolate);
  v8::Handle<v8::ObjectTemplate> global_template =
      v8::ObjectTemplate::New(isolate);

  global_template->SetAccessCheckCallbacks(AccessBlocker, NULL);

  // Add accessible accessor.
  global_template->SetAccessor(
      v8_str("accessible_prop"),
      EchoGetter, EchoSetter,
      v8::Handle<Value>(),
      v8::AccessControl(v8::ALL_CAN_READ | v8::ALL_CAN_WRITE));


  // Add an accessor that is not accessible by cross-domain JS code.
  global_template->SetAccessor(v8_str("blocked_prop"),
                               UnreachableGetter, UnreachableSetter,
                               v8::Handle<Value>(),
                               v8::DEFAULT);

  // Create an environment
  v8::Local<Context> context0 = Context::New(isolate, NULL, global_template);
  context0->Enter();

  v8::Handle<v8::Object> global0 = context0->Global();

  v8::Local<Context> context1 = Context::New(isolate);
  context1->Enter();
  v8::Handle<v8::Object> global1 = context1->Global();
  global1->Set(v8_str("other"), global0);

  // Regression test for issue 1154.
  CHECK(CompileRun("Object.keys(other)").IsEmpty());
  CHECK(CompileRun("other.blocked_prop").IsEmpty());

  // Regression test for issue 1027.
  CompileRun("Object.defineProperty(\n"
             "  other, 'blocked_prop', {configurable: false})");
  CHECK(CompileRun("other.blocked_prop").IsEmpty());
  CHECK(CompileRun("Object.getOwnPropertyDescriptor(other, 'blocked_prop')")
            .IsEmpty());

  // Regression test for issue 1171.
  ExpectTrue("Object.isExtensible(other)");
  CompileRun("Object.preventExtensions(other)");
  ExpectTrue("Object.isExtensible(other)");

  // Object.seal and Object.freeze.
  CompileRun("Object.freeze(other)");
  ExpectTrue("Object.isExtensible(other)");

  CompileRun("Object.seal(other)");
  ExpectTrue("Object.isExtensible(other)");

  // Regression test for issue 1250.
  // Make sure that we can set the accessible accessors value using normal
  // assignment.
  CompileRun("other.accessible_prop = 42");
  CHECK_EQ(42, g_echo_value);

  v8::Handle<Value> value;
  CompileRun("Object.defineProperty(other, 'accessible_prop', {value: -1})");
  value = CompileRun("other.accessible_prop == 42");
  CHECK(value->IsTrue());
}


static bool AccessAlwaysBlocked(Local<v8::Object> global, Local<Value> name,
                                v8::AccessType type, Local<Value> data) {
  i::PrintF("Access blocked.\n");
  return false;
}


THREADED_TEST(AccessControlGetOwnPropertyNames) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope handle_scope(isolate);
  v8::Handle<v8::ObjectTemplate> obj_template =
      v8::ObjectTemplate::New(isolate);

  obj_template->Set(v8_str("x"), v8::Integer::New(isolate, 42));
  obj_template->SetAccessCheckCallbacks(AccessAlwaysBlocked, NULL);

  // Create an environment
  v8::Local<Context> context0 = Context::New(isolate, NULL, obj_template);
  context0->Enter();

  v8::Handle<v8::Object> global0 = context0->Global();

  v8::HandleScope scope1(CcTest::isolate());

  v8::Local<Context> context1 = Context::New(isolate);
  context1->Enter();

  v8::Handle<v8::Object> global1 = context1->Global();
  global1->Set(v8_str("other"), global0);
  global1->Set(v8_str("object"), obj_template->NewInstance());

  v8::Handle<Value> value;

  // Attempt to get the property names of the other global object and
  // of an object that requires access checks.  Accessing the other
  // global object should be blocked by access checks on the global
  // proxy object.  Accessing the object that requires access checks
  // is blocked by the access checks on the object itself.
  value = CompileRun("Object.getOwnPropertyNames(other).length == 0");
  CHECK(value.IsEmpty());

  value = CompileRun("Object.getOwnPropertyNames(object).length == 0");
  CHECK(value.IsEmpty());

  context1->Exit();
  context0->Exit();
}


TEST(SuperAccessControl) {
  i::FLAG_allow_natives_syntax = true;
  i::FLAG_harmony_classes = true;
  i::FLAG_harmony_object_literals = true;
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope handle_scope(isolate);
  v8::Handle<v8::ObjectTemplate> obj_template =
      v8::ObjectTemplate::New(isolate);
  obj_template->SetAccessCheckCallbacks(AccessAlwaysBlocked, NULL);
  LocalContext env;
  env->Global()->Set(v8_str("prohibited"), obj_template->NewInstance());

  {
    v8::TryCatch try_catch;
    CompileRun(
        "var f = { m() { return super.hasOwnProperty; } }.m;"
        "var m = %ToMethod(f, prohibited);"
        "m();");
    CHECK(try_catch.HasCaught());
  }

  {
    v8::TryCatch try_catch;
    CompileRun(
        "var f = {m() { return super[42]; } }.m;"
        "var m = %ToMethod(f, prohibited);"
        "m();");
    CHECK(try_catch.HasCaught());
  }

  {
    v8::TryCatch try_catch;
    CompileRun(
        "var f = {m() { super.hasOwnProperty = function () {}; } }.m;"
        "var m = %ToMethod(f, prohibited);"
        "m();");
    CHECK(try_catch.HasCaught());
  }

  {
    v8::TryCatch try_catch;
    CompileRun(
        "Object.defineProperty(Object.prototype, 'x', { set : function(){}});"
        "var f = {"
        "  m() { "
        "    'use strict';"
        "    super.x = function () {};"
        "  }"
        "}.m;"
        "var m = %ToMethod(f, prohibited);"
        "m();");
    CHECK(try_catch.HasCaught());
  }
}


TEST(Regress470113) {
  i::FLAG_harmony_classes = true;
  i::FLAG_harmony_object_literals = true;
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope handle_scope(isolate);
  v8::Handle<v8::ObjectTemplate> obj_template =
      v8::ObjectTemplate::New(isolate);
  obj_template->SetAccessCheckCallbacks(AccessAlwaysBlocked, NULL);
  LocalContext env;
  env->Global()->Set(v8_str("prohibited"), obj_template->NewInstance());

  {
    v8::TryCatch try_catch;
    CompileRun(
        "'use strict';\n"
        "class C extends Object {\n"
        "   m() { super.powned = 'Powned!'; }\n"
        "}\n"
        "let c = new C();\n"
        "c.m.call(prohibited)");

    CHECK(try_catch.HasCaught());
  }
}


static void ConstTenGetter(Local<String> name,
                           const v8::PropertyCallbackInfo<v8::Value>& info) {
  info.GetReturnValue().Set(v8_num(10));
}


THREADED_TEST(CrossDomainAccessors) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope handle_scope(isolate);

  v8::Handle<v8::FunctionTemplate> func_template =
      v8::FunctionTemplate::New(isolate);

  v8::Handle<v8::ObjectTemplate> global_template =
      func_template->InstanceTemplate();

  v8::Handle<v8::ObjectTemplate> proto_template =
      func_template->PrototypeTemplate();

  // Add an accessor to proto that's accessible by cross-domain JS code.
  proto_template->SetAccessor(v8_str("accessible"),
                              ConstTenGetter, 0,
                              v8::Handle<Value>(),
                              v8::ALL_CAN_READ);

  // Add an accessor that is not accessible by cross-domain JS code.
  global_template->SetAccessor(v8_str("unreachable"),
                               UnreachableGetter, 0,
                               v8::Handle<Value>(),
                               v8::DEFAULT);

  v8::Local<Context> context0 = Context::New(isolate, NULL, global_template);
  context0->Enter();

  Local<v8::Object> global = context0->Global();
  // Add a normal property that shadows 'accessible'
  global->Set(v8_str("accessible"), v8_num(11));

  // Enter a new context.
  v8::HandleScope scope1(CcTest::isolate());
  v8::Local<Context> context1 = Context::New(isolate);
  context1->Enter();

  v8::Handle<v8::Object> global1 = context1->Global();
  global1->Set(v8_str("other"), global);

  // Should return 10, instead of 11
  v8::Handle<Value> value = v8_compile("other.accessible")->Run();
  CHECK(value->IsNumber());
  CHECK_EQ(10, value->Int32Value());

  value = v8_compile("other.unreachable")->Run();
  CHECK(value.IsEmpty());

  context1->Exit();
  context0->Exit();
}


static int access_count = 0;

static bool AccessCounter(Local<v8::Object> global, Local<Value> name,
                          v8::AccessType type, Local<Value> data) {
  access_count++;
  return true;
}


// This one is too easily disturbed by other tests.
TEST(AccessControlIC) {
  access_count = 0;

  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope handle_scope(isolate);

  // Create an environment.
  v8::Local<Context> context0 = Context::New(isolate);
  context0->Enter();

  // Create an object that requires access-check functions to be
  // called for cross-domain access.
  v8::Handle<v8::ObjectTemplate> object_template =
      v8::ObjectTemplate::New(isolate);
  object_template->SetAccessCheckCallbacks(AccessCounter, NULL);
  Local<v8::Object> object = object_template->NewInstance();

  v8::HandleScope scope1(isolate);

  // Create another environment.
  v8::Local<Context> context1 = Context::New(isolate);
  context1->Enter();

  // Make easy access to the object from the other environment.
  v8::Handle<v8::Object> global1 = context1->Global();
  global1->Set(v8_str("obj"), object);

  v8::Handle<Value> value;

  // Check that the named access-control function is called every time.
  CompileRun("function testProp(obj) {"
             "  for (var i = 0; i < 10; i++) obj.prop = 1;"
             "  for (var j = 0; j < 10; j++) obj.prop;"
             "  return obj.prop"
             "}");
  value = CompileRun("testProp(obj)");
  CHECK(value->IsNumber());
  CHECK_EQ(1, value->Int32Value());
  CHECK_EQ(21, access_count);

  // Check that the named access-control function is called every time.
  CompileRun("var p = 'prop';"
             "function testKeyed(obj) {"
             "  for (var i = 0; i < 10; i++) obj[p] = 1;"
             "  for (var j = 0; j < 10; j++) obj[p];"
             "  return obj[p];"
             "}");
  // Use obj which requires access checks.  No inline caching is used
  // in that case.
  value = CompileRun("testKeyed(obj)");
  CHECK(value->IsNumber());
  CHECK_EQ(1, value->Int32Value());
  CHECK_EQ(42, access_count);
  // Force the inline caches into generic state and try again.
  CompileRun("testKeyed({ a: 0 })");
  CompileRun("testKeyed({ b: 0 })");
  value = CompileRun("testKeyed(obj)");
  CHECK(value->IsNumber());
  CHECK_EQ(1, value->Int32Value());
  CHECK_EQ(63, access_count);

  // Check that the indexed access-control function is called every time.
  access_count = 0;

  CompileRun("function testIndexed(obj) {"
             "  for (var i = 0; i < 10; i++) obj[0] = 1;"
             "  for (var j = 0; j < 10; j++) obj[0];"
             "  return obj[0]"
             "}");
  value = CompileRun("testIndexed(obj)");
  CHECK(value->IsNumber());
  CHECK_EQ(1, value->Int32Value());
  CHECK_EQ(21, access_count);
  // Force the inline caches into generic state.
  CompileRun("testIndexed(new Array(1))");
  // Test that the indexed access check is called.
  value = CompileRun("testIndexed(obj)");
  CHECK(value->IsNumber());
  CHECK_EQ(1, value->Int32Value());
  CHECK_EQ(42, access_count);

  access_count = 0;
  // Check that the named access check is called when invoking
  // functions on an object that requires access checks.
  CompileRun("obj.f = function() {}");
  CompileRun("function testCallNormal(obj) {"
             "  for (var i = 0; i < 10; i++) obj.f();"
             "}");
  CompileRun("testCallNormal(obj)");
  printf("%i\n", access_count);
  CHECK_EQ(11, access_count);

  // Force obj into slow case.
  value = CompileRun("delete obj.prop");
  CHECK(value->BooleanValue());
  // Force inline caches into dictionary probing mode.
  CompileRun("var o = { x: 0 }; delete o.x; testProp(o);");
  // Test that the named access check is called.
  value = CompileRun("testProp(obj);");
  CHECK(value->IsNumber());
  CHECK_EQ(1, value->Int32Value());
  CHECK_EQ(33, access_count);

  // Force the call inline cache into dictionary probing mode.
  CompileRun("o.f = function() {}; testCallNormal(o)");
  // Test that the named access check is still called for each
  // invocation of the function.
  value = CompileRun("testCallNormal(obj)");
  CHECK_EQ(43, access_count);

  context1->Exit();
  context0->Exit();
}


THREADED_TEST(Version) { v8::V8::GetVersion(); }


static void InstanceFunctionCallback(
    const v8::FunctionCallbackInfo<v8::Value>& args) {
  ApiTestFuzzer::Fuzz();
  args.GetReturnValue().Set(v8_num(12));
}


THREADED_TEST(InstanceProperties) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope handle_scope(isolate);

  Local<v8::FunctionTemplate> t = v8::FunctionTemplate::New(isolate);
  Local<ObjectTemplate> instance = t->InstanceTemplate();

  instance->Set(v8_str("x"), v8_num(42));
  instance->Set(v8_str("f"),
                v8::FunctionTemplate::New(isolate, InstanceFunctionCallback));

  Local<Value> o = t->GetFunction()->NewInstance();

  context->Global()->Set(v8_str("i"), o);
  Local<Value> value = CompileRun("i.x");
  CHECK_EQ(42, value->Int32Value());

  value = CompileRun("i.f()");
  CHECK_EQ(12, value->Int32Value());
}


static void GlobalObjectInstancePropertiesGet(
    Local<Name> key, const v8::PropertyCallbackInfo<v8::Value>&) {
  ApiTestFuzzer::Fuzz();
}


THREADED_TEST(GlobalObjectInstanceProperties) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope handle_scope(isolate);

  Local<Value> global_object;

  Local<v8::FunctionTemplate> t = v8::FunctionTemplate::New(isolate);
  t->InstanceTemplate()->SetHandler(
      v8::NamedPropertyHandlerConfiguration(GlobalObjectInstancePropertiesGet));
  Local<ObjectTemplate> instance_template = t->InstanceTemplate();
  instance_template->Set(v8_str("x"), v8_num(42));
  instance_template->Set(v8_str("f"),
                         v8::FunctionTemplate::New(isolate,
                                                   InstanceFunctionCallback));

  // The script to check how Crankshaft compiles missing global function
  // invocations.  function g is not defined and should throw on call.
  const char* script =
      "function wrapper(call) {"
      "  var x = 0, y = 1;"
      "  for (var i = 0; i < 1000; i++) {"
      "    x += i * 100;"
      "    y += i * 100;"
      "  }"
      "  if (call) g();"
      "}"
      "for (var i = 0; i < 17; i++) wrapper(false);"
      "var thrown = 0;"
      "try { wrapper(true); } catch (e) { thrown = 1; };"
      "thrown";

  {
    LocalContext env(NULL, instance_template);
    // Hold on to the global object so it can be used again in another
    // environment initialization.
    global_object = env->Global();

    Local<Value> value = CompileRun("x");
    CHECK_EQ(42, value->Int32Value());
    value = CompileRun("f()");
    CHECK_EQ(12, value->Int32Value());
    value = CompileRun(script);
    CHECK_EQ(1, value->Int32Value());
  }

  {
    // Create new environment reusing the global object.
    LocalContext env(NULL, instance_template, global_object);
    Local<Value> value = CompileRun("x");
    CHECK_EQ(42, value->Int32Value());
    value = CompileRun("f()");
    CHECK_EQ(12, value->Int32Value());
    value = CompileRun(script);
    CHECK_EQ(1, value->Int32Value());
  }
}


THREADED_TEST(CallKnownGlobalReceiver) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope handle_scope(isolate);

  Local<Value> global_object;

  Local<v8::FunctionTemplate> t = v8::FunctionTemplate::New(isolate);
  Local<ObjectTemplate> instance_template = t->InstanceTemplate();

  // The script to check that we leave global object not
  // global object proxy on stack when we deoptimize from inside
  // arguments evaluation.
  // To provoke error we need to both force deoptimization
  // from arguments evaluation and to force CallIC to take
  // CallIC_Miss code path that can't cope with global proxy.
  const char* script =
      "function bar(x, y) { try { } finally { } }"
      "function baz(x) { try { } finally { } }"
      "function bom(x) { try { } finally { } }"
      "function foo(x) { bar([x], bom(2)); }"
      "for (var i = 0; i < 10000; i++) foo(1);"
      "foo";

  Local<Value> foo;
  {
    LocalContext env(NULL, instance_template);
    // Hold on to the global object so it can be used again in another
    // environment initialization.
    global_object = env->Global();
    foo = CompileRun(script);
  }

  {
    // Create new environment reusing the global object.
    LocalContext env(NULL, instance_template, global_object);
    env->Global()->Set(v8_str("foo"), foo);
    CompileRun("foo()");
  }
}


static void ShadowFunctionCallback(
    const v8::FunctionCallbackInfo<v8::Value>& args) {
  ApiTestFuzzer::Fuzz();
  args.GetReturnValue().Set(v8_num(42));
}


static int shadow_y;
static int shadow_y_setter_call_count;
static int shadow_y_getter_call_count;


static void ShadowYSetter(Local<String>,
                          Local<Value>,
                          const v8::PropertyCallbackInfo<void>&) {
  shadow_y_setter_call_count++;
  shadow_y = 42;
}


static void ShadowYGetter(Local<String> name,
                          const v8::PropertyCallbackInfo<v8::Value>& info) {
  ApiTestFuzzer::Fuzz();
  shadow_y_getter_call_count++;
  info.GetReturnValue().Set(v8_num(shadow_y));
}


static void ShadowIndexedGet(uint32_t index,
                             const v8::PropertyCallbackInfo<v8::Value>&) {
}


static void ShadowNamedGet(Local<Name> key,
                           const v8::PropertyCallbackInfo<v8::Value>&) {}


THREADED_TEST(ShadowObject) {
  shadow_y = shadow_y_setter_call_count = shadow_y_getter_call_count = 0;
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope handle_scope(isolate);

  Local<ObjectTemplate> global_template = v8::ObjectTemplate::New(isolate);
  LocalContext context(NULL, global_template);

  Local<v8::FunctionTemplate> t = v8::FunctionTemplate::New(isolate);
  t->InstanceTemplate()->SetHandler(
      v8::NamedPropertyHandlerConfiguration(ShadowNamedGet));
  t->InstanceTemplate()->SetHandler(
      v8::IndexedPropertyHandlerConfiguration(ShadowIndexedGet));
  Local<ObjectTemplate> proto = t->PrototypeTemplate();
  Local<ObjectTemplate> instance = t->InstanceTemplate();

  proto->Set(v8_str("f"),
             v8::FunctionTemplate::New(isolate,
                                       ShadowFunctionCallback,
                                       Local<Value>()));
  proto->Set(v8_str("x"), v8_num(12));

  instance->SetAccessor(v8_str("y"), ShadowYGetter, ShadowYSetter);

  Local<Value> o = t->GetFunction()->NewInstance();
  context->Global()->Set(v8_str("__proto__"), o);

  Local<Value> value =
      CompileRun("this.propertyIsEnumerable(0)");
  CHECK(value->IsBoolean());
  CHECK(!value->BooleanValue());

  value = CompileRun("x");
  CHECK_EQ(12, value->Int32Value());

  value = CompileRun("f()");
  CHECK_EQ(42, value->Int32Value());

  CompileRun("y = 43");
  CHECK_EQ(1, shadow_y_setter_call_count);
  value = CompileRun("y");
  CHECK_EQ(1, shadow_y_getter_call_count);
  CHECK_EQ(42, value->Int32Value());
}


THREADED_TEST(HiddenPrototype) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope handle_scope(isolate);

  Local<v8::FunctionTemplate> t0 = v8::FunctionTemplate::New(isolate);
  t0->InstanceTemplate()->Set(v8_str("x"), v8_num(0));
  Local<v8::FunctionTemplate> t1 = v8::FunctionTemplate::New(isolate);
  t1->SetHiddenPrototype(true);
  t1->InstanceTemplate()->Set(v8_str("y"), v8_num(1));
  Local<v8::FunctionTemplate> t2 = v8::FunctionTemplate::New(isolate);
  t2->SetHiddenPrototype(true);
  t2->InstanceTemplate()->Set(v8_str("z"), v8_num(2));
  Local<v8::FunctionTemplate> t3 = v8::FunctionTemplate::New(isolate);
  t3->InstanceTemplate()->Set(v8_str("u"), v8_num(3));

  Local<v8::Object> o0 = t0->GetFunction()->NewInstance();
  Local<v8::Object> o1 = t1->GetFunction()->NewInstance();
  Local<v8::Object> o2 = t2->GetFunction()->NewInstance();
  Local<v8::Object> o3 = t3->GetFunction()->NewInstance();

  // Setting the prototype on an object skips hidden prototypes.
  CHECK_EQ(0, o0->Get(v8_str("x"))->Int32Value());
  o0->Set(v8_str("__proto__"), o1);
  CHECK_EQ(0, o0->Get(v8_str("x"))->Int32Value());
  CHECK_EQ(1, o0->Get(v8_str("y"))->Int32Value());
  o0->Set(v8_str("__proto__"), o2);
  CHECK_EQ(0, o0->Get(v8_str("x"))->Int32Value());
  CHECK_EQ(1, o0->Get(v8_str("y"))->Int32Value());
  CHECK_EQ(2, o0->Get(v8_str("z"))->Int32Value());
  o0->Set(v8_str("__proto__"), o3);
  CHECK_EQ(0, o0->Get(v8_str("x"))->Int32Value());
  CHECK_EQ(1, o0->Get(v8_str("y"))->Int32Value());
  CHECK_EQ(2, o0->Get(v8_str("z"))->Int32Value());
  CHECK_EQ(3, o0->Get(v8_str("u"))->Int32Value());

  // Getting the prototype of o0 should get the first visible one
  // which is o3.  Therefore, z should not be defined on the prototype
  // object.
  Local<Value> proto = o0->Get(v8_str("__proto__"));
  CHECK(proto->IsObject());
  CHECK(proto.As<v8::Object>()->Get(v8_str("z"))->IsUndefined());
}


THREADED_TEST(HiddenPrototypeSet) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope handle_scope(isolate);

  Local<v8::FunctionTemplate> ot = v8::FunctionTemplate::New(isolate);
  Local<v8::FunctionTemplate> ht = v8::FunctionTemplate::New(isolate);
  ht->SetHiddenPrototype(true);
  Local<v8::FunctionTemplate> pt = v8::FunctionTemplate::New(isolate);
  ht->InstanceTemplate()->Set(v8_str("x"), v8_num(0));

  Local<v8::Object> o = ot->GetFunction()->NewInstance();
  Local<v8::Object> h = ht->GetFunction()->NewInstance();
  Local<v8::Object> p = pt->GetFunction()->NewInstance();
  o->Set(v8_str("__proto__"), h);
  h->Set(v8_str("__proto__"), p);

  // Setting a property that exists on the hidden prototype goes there.
  o->Set(v8_str("x"), v8_num(7));
  CHECK_EQ(7, o->Get(v8_str("x"))->Int32Value());
  CHECK_EQ(7, h->Get(v8_str("x"))->Int32Value());
  CHECK(p->Get(v8_str("x"))->IsUndefined());

  // Setting a new property should not be forwarded to the hidden prototype.
  o->Set(v8_str("y"), v8_num(6));
  CHECK_EQ(6, o->Get(v8_str("y"))->Int32Value());
  CHECK(h->Get(v8_str("y"))->IsUndefined());
  CHECK(p->Get(v8_str("y"))->IsUndefined());

  // Setting a property that only exists on a prototype of the hidden prototype
  // is treated normally again.
  p->Set(v8_str("z"), v8_num(8));
  CHECK_EQ(8, o->Get(v8_str("z"))->Int32Value());
  CHECK_EQ(8, h->Get(v8_str("z"))->Int32Value());
  CHECK_EQ(8, p->Get(v8_str("z"))->Int32Value());
  o->Set(v8_str("z"), v8_num(9));
  CHECK_EQ(9, o->Get(v8_str("z"))->Int32Value());
  CHECK_EQ(8, h->Get(v8_str("z"))->Int32Value());
  CHECK_EQ(8, p->Get(v8_str("z"))->Int32Value());
}


// Regression test for issue 2457.
THREADED_TEST(HiddenPrototypeIdentityHash) {
  LocalContext context;
  v8::HandleScope handle_scope(context->GetIsolate());

  Handle<FunctionTemplate> t = FunctionTemplate::New(context->GetIsolate());
  t->SetHiddenPrototype(true);
  t->InstanceTemplate()->Set(v8_str("foo"), v8_num(75));
  Handle<Object> p = t->GetFunction()->NewInstance();
  Handle<Object> o = Object::New(context->GetIsolate());
  o->SetPrototype(p);

  int hash = o->GetIdentityHash();
  USE(hash);
  o->Set(v8_str("foo"), v8_num(42));
  DCHECK_EQ(hash, o->GetIdentityHash());
}


THREADED_TEST(SetPrototype) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope handle_scope(isolate);

  Local<v8::FunctionTemplate> t0 = v8::FunctionTemplate::New(isolate);
  t0->InstanceTemplate()->Set(v8_str("x"), v8_num(0));
  Local<v8::FunctionTemplate> t1 = v8::FunctionTemplate::New(isolate);
  t1->SetHiddenPrototype(true);
  t1->InstanceTemplate()->Set(v8_str("y"), v8_num(1));
  Local<v8::FunctionTemplate> t2 = v8::FunctionTemplate::New(isolate);
  t2->SetHiddenPrototype(true);
  t2->InstanceTemplate()->Set(v8_str("z"), v8_num(2));
  Local<v8::FunctionTemplate> t3 = v8::FunctionTemplate::New(isolate);
  t3->InstanceTemplate()->Set(v8_str("u"), v8_num(3));

  Local<v8::Object> o0 = t0->GetFunction()->NewInstance();
  Local<v8::Object> o1 = t1->GetFunction()->NewInstance();
  Local<v8::Object> o2 = t2->GetFunction()->NewInstance();
  Local<v8::Object> o3 = t3->GetFunction()->NewInstance();

  // Setting the prototype on an object does not skip hidden prototypes.
  CHECK_EQ(0, o0->Get(v8_str("x"))->Int32Value());
  CHECK(o0->SetPrototype(o1));
  CHECK_EQ(0, o0->Get(v8_str("x"))->Int32Value());
  CHECK_EQ(1, o0->Get(v8_str("y"))->Int32Value());
  CHECK(o1->SetPrototype(o2));
  CHECK_EQ(0, o0->Get(v8_str("x"))->Int32Value());
  CHECK_EQ(1, o0->Get(v8_str("y"))->Int32Value());
  CHECK_EQ(2, o0->Get(v8_str("z"))->Int32Value());
  CHECK(o2->SetPrototype(o3));
  CHECK_EQ(0, o0->Get(v8_str("x"))->Int32Value());
  CHECK_EQ(1, o0->Get(v8_str("y"))->Int32Value());
  CHECK_EQ(2, o0->Get(v8_str("z"))->Int32Value());
  CHECK_EQ(3, o0->Get(v8_str("u"))->Int32Value());

  // Getting the prototype of o0 should get the first visible one
  // which is o3.  Therefore, z should not be defined on the prototype
  // object.
  Local<Value> proto = o0->Get(v8_str("__proto__"));
  CHECK(proto->IsObject());
  CHECK(proto.As<v8::Object>()->Equals(o3));

  // However, Object::GetPrototype ignores hidden prototype.
  Local<Value> proto0 = o0->GetPrototype();
  CHECK(proto0->IsObject());
  CHECK(proto0.As<v8::Object>()->Equals(o1));

  Local<Value> proto1 = o1->GetPrototype();
  CHECK(proto1->IsObject());
  CHECK(proto1.As<v8::Object>()->Equals(o2));

  Local<Value> proto2 = o2->GetPrototype();
  CHECK(proto2->IsObject());
  CHECK(proto2.As<v8::Object>()->Equals(o3));
}


// Getting property names of an object with a prototype chain that
// triggers dictionary elements in GetOwnPropertyNames() shouldn't
// crash the runtime.
THREADED_TEST(Regress91517) {
  i::FLAG_allow_natives_syntax = true;
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope handle_scope(isolate);

  Local<v8::FunctionTemplate> t1 = v8::FunctionTemplate::New(isolate);
  t1->SetHiddenPrototype(true);
  t1->InstanceTemplate()->Set(v8_str("foo"), v8_num(1));
  Local<v8::FunctionTemplate> t2 = v8::FunctionTemplate::New(isolate);
  t2->SetHiddenPrototype(true);
  t2->InstanceTemplate()->Set(v8_str("fuz1"), v8_num(2));
  t2->InstanceTemplate()->Set(v8_str("objects"), v8::Object::New(isolate));
  t2->InstanceTemplate()->Set(v8_str("fuz2"), v8_num(2));
  Local<v8::FunctionTemplate> t3 = v8::FunctionTemplate::New(isolate);
  t3->SetHiddenPrototype(true);
  t3->InstanceTemplate()->Set(v8_str("boo"), v8_num(3));
  Local<v8::FunctionTemplate> t4 = v8::FunctionTemplate::New(isolate);
  t4->InstanceTemplate()->Set(v8_str("baz"), v8_num(4));

  // Force dictionary-based properties.
  i::ScopedVector<char> name_buf(1024);
  for (int i = 1; i <= 1000; i++) {
    i::SNPrintF(name_buf, "sdf%d", i);
    t2->InstanceTemplate()->Set(v8_str(name_buf.start()), v8_num(2));
  }

  Local<v8::Object> o1 = t1->GetFunction()->NewInstance();
  Local<v8::Object> o2 = t2->GetFunction()->NewInstance();
  Local<v8::Object> o3 = t3->GetFunction()->NewInstance();
  Local<v8::Object> o4 = t4->GetFunction()->NewInstance();

  // Create prototype chain of hidden prototypes.
  CHECK(o4->SetPrototype(o3));
  CHECK(o3->SetPrototype(o2));
  CHECK(o2->SetPrototype(o1));

  // Call the runtime version of GetOwnPropertyNames() on the natively
  // created object through JavaScript.
  context->Global()->Set(v8_str("obj"), o4);
  // PROPERTY_ATTRIBUTES_NONE = 0
  CompileRun("var names = %GetOwnPropertyNames(obj, 0);");

  ExpectInt32("names.length", 1006);
  ExpectTrue("names.indexOf(\"baz\") >= 0");
  ExpectTrue("names.indexOf(\"boo\") >= 0");
  ExpectTrue("names.indexOf(\"foo\") >= 0");
  ExpectTrue("names.indexOf(\"fuz1\") >= 0");
  ExpectTrue("names.indexOf(\"fuz2\") >= 0");
  ExpectFalse("names[1005] == undefined");
}


// Getting property names of an object with a hidden and inherited
// prototype should not duplicate the accessor properties inherited.
THREADED_TEST(Regress269562) {
  i::FLAG_allow_natives_syntax = true;
  LocalContext context;
  v8::HandleScope handle_scope(context->GetIsolate());

  Local<v8::FunctionTemplate> t1 =
      v8::FunctionTemplate::New(context->GetIsolate());
  t1->SetHiddenPrototype(true);

  Local<v8::ObjectTemplate> i1 = t1->InstanceTemplate();
  i1->SetAccessor(v8_str("foo"),
                  SimpleAccessorGetter, SimpleAccessorSetter);
  i1->SetAccessor(v8_str("bar"),
                  SimpleAccessorGetter, SimpleAccessorSetter);
  i1->SetAccessor(v8_str("baz"),
                  SimpleAccessorGetter, SimpleAccessorSetter);
  i1->Set(v8_str("n1"), v8_num(1));
  i1->Set(v8_str("n2"), v8_num(2));

  Local<v8::Object> o1 = t1->GetFunction()->NewInstance();
  Local<v8::FunctionTemplate> t2 =
      v8::FunctionTemplate::New(context->GetIsolate());
  t2->SetHiddenPrototype(true);

  // Inherit from t1 and mark prototype as hidden.
  t2->Inherit(t1);
  t2->InstanceTemplate()->Set(v8_str("mine"), v8_num(4));

  Local<v8::Object> o2 = t2->GetFunction()->NewInstance();
  CHECK(o2->SetPrototype(o1));

  v8::Local<v8::Symbol> sym =
      v8::Symbol::New(context->GetIsolate(), v8_str("s1"));
  o1->Set(sym, v8_num(3));
  o1->SetHiddenValue(
      v8_str("h1"), v8::Integer::New(context->GetIsolate(), 2013));

  // Call the runtime version of GetOwnPropertyNames() on
  // the natively created object through JavaScript.
  context->Global()->Set(v8_str("obj"), o2);
  context->Global()->Set(v8_str("sym"), sym);
  // PROPERTY_ATTRIBUTES_NONE = 0
  CompileRun("var names = %GetOwnPropertyNames(obj, 0);");

  ExpectInt32("names.length", 7);
  ExpectTrue("names.indexOf(\"foo\") >= 0");
  ExpectTrue("names.indexOf(\"bar\") >= 0");
  ExpectTrue("names.indexOf(\"baz\") >= 0");
  ExpectTrue("names.indexOf(\"n1\") >= 0");
  ExpectTrue("names.indexOf(\"n2\") >= 0");
  ExpectTrue("names.indexOf(sym) >= 0");
  ExpectTrue("names.indexOf(\"mine\") >= 0");
}


THREADED_TEST(FunctionReadOnlyPrototype) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope handle_scope(isolate);

  Local<v8::FunctionTemplate> t1 = v8::FunctionTemplate::New(isolate);
  t1->PrototypeTemplate()->Set(v8_str("x"), v8::Integer::New(isolate, 42));
  t1->ReadOnlyPrototype();
  context->Global()->Set(v8_str("func1"), t1->GetFunction());
  // Configured value of ReadOnly flag.
  CHECK(CompileRun(
      "(function() {"
      "  descriptor = Object.getOwnPropertyDescriptor(func1, 'prototype');"
      "  return (descriptor['writable'] == false);"
      "})()")->BooleanValue());
  CHECK_EQ(42, CompileRun("func1.prototype.x")->Int32Value());
  CHECK_EQ(42,
           CompileRun("func1.prototype = {}; func1.prototype.x")->Int32Value());

  Local<v8::FunctionTemplate> t2 = v8::FunctionTemplate::New(isolate);
  t2->PrototypeTemplate()->Set(v8_str("x"), v8::Integer::New(isolate, 42));
  context->Global()->Set(v8_str("func2"), t2->GetFunction());
  // Default value of ReadOnly flag.
  CHECK(CompileRun(
      "(function() {"
      "  descriptor = Object.getOwnPropertyDescriptor(func2, 'prototype');"
      "  return (descriptor['writable'] == true);"
      "})()")->BooleanValue());
  CHECK_EQ(42, CompileRun("func2.prototype.x")->Int32Value());
}


THREADED_TEST(SetPrototypeThrows) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope handle_scope(isolate);

  Local<v8::FunctionTemplate> t = v8::FunctionTemplate::New(isolate);

  Local<v8::Object> o0 = t->GetFunction()->NewInstance();
  Local<v8::Object> o1 = t->GetFunction()->NewInstance();

  CHECK(o0->SetPrototype(o1));
  // If setting the prototype leads to the cycle, SetPrototype should
  // return false and keep VM in sane state.
  v8::TryCatch try_catch;
  CHECK(!o1->SetPrototype(o0));
  CHECK(!try_catch.HasCaught());
  DCHECK(!CcTest::i_isolate()->has_pending_exception());

  CHECK_EQ(42, CompileRun("function f() { return 42; }; f()")->Int32Value());
}


THREADED_TEST(FunctionRemovePrototype) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope handle_scope(isolate);

  Local<v8::FunctionTemplate> t1 = v8::FunctionTemplate::New(isolate);
  t1->RemovePrototype();
  Local<v8::Function> fun = t1->GetFunction();
  context->Global()->Set(v8_str("fun"), fun);
  CHECK(!CompileRun("'prototype' in fun")->BooleanValue());

  v8::TryCatch try_catch;
  CompileRun("new fun()");
  CHECK(try_catch.HasCaught());

  try_catch.Reset();
  fun->NewInstance();
  CHECK(try_catch.HasCaught());
}


THREADED_TEST(GetterSetterExceptions) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope handle_scope(isolate);
  CompileRun(
      "function Foo() { };"
      "function Throw() { throw 5; };"
      "var x = { };"
      "x.__defineSetter__('set', Throw);"
      "x.__defineGetter__('get', Throw);");
  Local<v8::Object> x =
      Local<v8::Object>::Cast(context->Global()->Get(v8_str("x")));
  v8::TryCatch try_catch;
  x->Set(v8_str("set"), v8::Integer::New(isolate, 8));
  x->Get(v8_str("get"));
  x->Set(v8_str("set"), v8::Integer::New(isolate, 8));
  x->Get(v8_str("get"));
  x->Set(v8_str("set"), v8::Integer::New(isolate, 8));
  x->Get(v8_str("get"));
  x->Set(v8_str("set"), v8::Integer::New(isolate, 8));
  x->Get(v8_str("get"));
}


THREADED_TEST(Constructor) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope handle_scope(isolate);
  Local<v8::FunctionTemplate> templ = v8::FunctionTemplate::New(isolate);
  templ->SetClassName(v8_str("Fun"));
  Local<Function> cons = templ->GetFunction();
  context->Global()->Set(v8_str("Fun"), cons);
  Local<v8::Object> inst = cons->NewInstance();
  i::Handle<i::JSObject> obj(v8::Utils::OpenHandle(*inst));
  CHECK(obj->IsJSObject());
  Local<Value> value = CompileRun("(new Fun()).constructor === Fun");
  CHECK(value->BooleanValue());
}


static void ConstructorCallback(
    const v8::FunctionCallbackInfo<v8::Value>& args) {
  ApiTestFuzzer::Fuzz();
  Local<Object> This;

  if (args.IsConstructCall()) {
    Local<Object> Holder = args.Holder();
    This = Object::New(args.GetIsolate());
    Local<Value> proto = Holder->GetPrototype();
    if (proto->IsObject()) {
      This->SetPrototype(proto);
    }
  } else {
    This = args.This();
  }

  This->Set(v8_str("a"), args[0]);
  args.GetReturnValue().Set(This);
}


static void FakeConstructorCallback(
    const v8::FunctionCallbackInfo<v8::Value>& args) {
  ApiTestFuzzer::Fuzz();
  args.GetReturnValue().Set(args[0]);
}


THREADED_TEST(ConstructorForObject) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope handle_scope(isolate);

  {
    Local<ObjectTemplate> instance_template = ObjectTemplate::New(isolate);
    instance_template->SetCallAsFunctionHandler(ConstructorCallback);
    Local<Object> instance = instance_template->NewInstance();
    context->Global()->Set(v8_str("obj"), instance);
    v8::TryCatch try_catch;
    Local<Value> value;
    CHECK(!try_catch.HasCaught());

    // Call the Object's constructor with a 32-bit signed integer.
    value = CompileRun("(function() { var o = new obj(28); return o.a; })()");
    CHECK(!try_catch.HasCaught());
    CHECK(value->IsInt32());
    CHECK_EQ(28, value->Int32Value());

    Local<Value> args1[] = {v8_num(28)};
    Local<Value> value_obj1 = instance->CallAsConstructor(1, args1);
    CHECK(value_obj1->IsObject());
    Local<Object> object1 = Local<Object>::Cast(value_obj1);
    value = object1->Get(v8_str("a"));
    CHECK(value->IsInt32());
    CHECK(!try_catch.HasCaught());
    CHECK_EQ(28, value->Int32Value());

    // Call the Object's constructor with a String.
    value =
        CompileRun("(function() { var o = new obj('tipli'); return o.a; })()");
    CHECK(!try_catch.HasCaught());
    CHECK(value->IsString());
    String::Utf8Value string_value1(value->ToString(isolate));
    CHECK_EQ(0, strcmp("tipli", *string_value1));

    Local<Value> args2[] = {v8_str("tipli")};
    Local<Value> value_obj2 = instance->CallAsConstructor(1, args2);
    CHECK(value_obj2->IsObject());
    Local<Object> object2 = Local<Object>::Cast(value_obj2);
    value = object2->Get(v8_str("a"));
    CHECK(!try_catch.HasCaught());
    CHECK(value->IsString());
    String::Utf8Value string_value2(value->ToString(isolate));
    CHECK_EQ(0, strcmp("tipli", *string_value2));

    // Call the Object's constructor with a Boolean.
    value = CompileRun("(function() { var o = new obj(true); return o.a; })()");
    CHECK(!try_catch.HasCaught());
    CHECK(value->IsBoolean());
    CHECK_EQ(true, value->BooleanValue());

    Handle<Value> args3[] = {v8::True(isolate)};
    Local<Value> value_obj3 = instance->CallAsConstructor(1, args3);
    CHECK(value_obj3->IsObject());
    Local<Object> object3 = Local<Object>::Cast(value_obj3);
    value = object3->Get(v8_str("a"));
    CHECK(!try_catch.HasCaught());
    CHECK(value->IsBoolean());
    CHECK_EQ(true, value->BooleanValue());

    // Call the Object's constructor with undefined.
    Handle<Value> args4[] = {v8::Undefined(isolate)};
    Local<Value> value_obj4 = instance->CallAsConstructor(1, args4);
    CHECK(value_obj4->IsObject());
    Local<Object> object4 = Local<Object>::Cast(value_obj4);
    value = object4->Get(v8_str("a"));
    CHECK(!try_catch.HasCaught());
    CHECK(value->IsUndefined());

    // Call the Object's constructor with null.
    Handle<Value> args5[] = {v8::Null(isolate)};
    Local<Value> value_obj5 = instance->CallAsConstructor(1, args5);
    CHECK(value_obj5->IsObject());
    Local<Object> object5 = Local<Object>::Cast(value_obj5);
    value = object5->Get(v8_str("a"));
    CHECK(!try_catch.HasCaught());
    CHECK(value->IsNull());
  }

  // Check exception handling when there is no constructor set for the Object.
  {
    Local<ObjectTemplate> instance_template = ObjectTemplate::New(isolate);
    Local<Object> instance = instance_template->NewInstance();
    context->Global()->Set(v8_str("obj2"), instance);
    v8::TryCatch try_catch;
    Local<Value> value;
    CHECK(!try_catch.HasCaught());

    value = CompileRun("new obj2(28)");
    CHECK(try_catch.HasCaught());
    String::Utf8Value exception_value1(try_catch.Exception());
    CHECK_EQ(0, strcmp("TypeError: obj2 is not a function", *exception_value1));
    try_catch.Reset();

    Local<Value> args[] = {v8_num(29)};
    value = instance->CallAsConstructor(1, args);
    CHECK(try_catch.HasCaught());
    String::Utf8Value exception_value2(try_catch.Exception());
    CHECK_EQ(
        0, strcmp("TypeError: #<Object> is not a function", *exception_value2));
    try_catch.Reset();
  }

  // Check the case when constructor throws exception.
  {
    Local<ObjectTemplate> instance_template = ObjectTemplate::New(isolate);
    instance_template->SetCallAsFunctionHandler(ThrowValue);
    Local<Object> instance = instance_template->NewInstance();
    context->Global()->Set(v8_str("obj3"), instance);
    v8::TryCatch try_catch;
    Local<Value> value;
    CHECK(!try_catch.HasCaught());

    value = CompileRun("new obj3(22)");
    CHECK(try_catch.HasCaught());
    String::Utf8Value exception_value1(try_catch.Exception());
    CHECK_EQ(0, strcmp("22", *exception_value1));
    try_catch.Reset();

    Local<Value> args[] = {v8_num(23)};
    value = instance->CallAsConstructor(1, args);
    CHECK(try_catch.HasCaught());
    String::Utf8Value exception_value2(try_catch.Exception());
    CHECK_EQ(0, strcmp("23", *exception_value2));
    try_catch.Reset();
  }

  // Check whether constructor returns with an object or non-object.
  {
    Local<FunctionTemplate> function_template =
        FunctionTemplate::New(isolate, FakeConstructorCallback);
    Local<Function> function = function_template->GetFunction();
    Local<Object> instance1 = function;
    context->Global()->Set(v8_str("obj4"), instance1);
    v8::TryCatch try_catch;
    Local<Value> value;
    CHECK(!try_catch.HasCaught());

    CHECK(instance1->IsObject());
    CHECK(instance1->IsFunction());

    value = CompileRun("new obj4(28)");
    CHECK(!try_catch.HasCaught());
    CHECK(value->IsObject());

    Local<Value> args1[] = {v8_num(28)};
    value = instance1->CallAsConstructor(1, args1);
    CHECK(!try_catch.HasCaught());
    CHECK(value->IsObject());

    Local<ObjectTemplate> instance_template = ObjectTemplate::New(isolate);
    instance_template->SetCallAsFunctionHandler(FakeConstructorCallback);
    Local<Object> instance2 = instance_template->NewInstance();
    context->Global()->Set(v8_str("obj5"), instance2);
    CHECK(!try_catch.HasCaught());

    CHECK(instance2->IsObject());
    CHECK(!instance2->IsFunction());

    value = CompileRun("new obj5(28)");
    CHECK(!try_catch.HasCaught());
    CHECK(!value->IsObject());

    Local<Value> args2[] = {v8_num(28)};
    value = instance2->CallAsConstructor(1, args2);
    CHECK(!try_catch.HasCaught());
    CHECK(!value->IsObject());
  }
}


THREADED_TEST(FunctionDescriptorException) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope handle_scope(isolate);
  Local<v8::FunctionTemplate> templ = v8::FunctionTemplate::New(isolate);
  templ->SetClassName(v8_str("Fun"));
  Local<Function> cons = templ->GetFunction();
  context->Global()->Set(v8_str("Fun"), cons);
  Local<Value> value = CompileRun(
      "function test() {"
      "  try {"
      "    (new Fun()).blah()"
      "  } catch (e) {"
      "    var str = String(e);"
      // "    if (str.indexOf('TypeError') == -1) return 1;"
      // "    if (str.indexOf('[object Fun]') != -1) return 2;"
      // "    if (str.indexOf('#<Fun>') == -1) return 3;"
      "    return 0;"
      "  }"
      "  return 4;"
      "}"
      "test();");
  CHECK_EQ(0, value->Int32Value());
}


THREADED_TEST(EvalAliasedDynamic) {
  LocalContext current;
  v8::HandleScope scope(current->GetIsolate());

  // Tests where aliased eval can only be resolved dynamically.
  Local<Script> script = v8_compile(
      "function f(x) { "
      "  var foo = 2;"
      "  with (x) { return eval('foo'); }"
      "}"
      "foo = 0;"
      "result1 = f(new Object());"
      "result2 = f(this);"
      "var x = new Object();"
      "x.eval = function(x) { return 1; };"
      "result3 = f(x);");
  script->Run();
  CHECK_EQ(2, current->Global()->Get(v8_str("result1"))->Int32Value());
  CHECK_EQ(0, current->Global()->Get(v8_str("result2"))->Int32Value());
  CHECK_EQ(1, current->Global()->Get(v8_str("result3"))->Int32Value());

  v8::TryCatch try_catch;
  script = v8_compile(
      "function f(x) { "
      "  var bar = 2;"
      "  with (x) { return eval('bar'); }"
      "}"
      "result4 = f(this)");
  script->Run();
  CHECK(!try_catch.HasCaught());
  CHECK_EQ(2, current->Global()->Get(v8_str("result4"))->Int32Value());

  try_catch.Reset();
}


THREADED_TEST(CrossEval) {
  v8::HandleScope scope(CcTest::isolate());
  LocalContext other;
  LocalContext current;

  Local<String> token = v8_str("<security token>");
  other->SetSecurityToken(token);
  current->SetSecurityToken(token);

  // Set up reference from current to other.
  current->Global()->Set(v8_str("other"), other->Global());

  // Check that new variables are introduced in other context.
  Local<Script> script = v8_compile("other.eval('var foo = 1234')");
  script->Run();
  Local<Value> foo = other->Global()->Get(v8_str("foo"));
  CHECK_EQ(1234, foo->Int32Value());
  CHECK(!current->Global()->Has(v8_str("foo")));

  // Check that writing to non-existing properties introduces them in
  // the other context.
  script = v8_compile("other.eval('na = 1234')");
  script->Run();
  CHECK_EQ(1234, other->Global()->Get(v8_str("na"))->Int32Value());
  CHECK(!current->Global()->Has(v8_str("na")));

  // Check that global variables in current context are not visible in other
  // context.
  v8::TryCatch try_catch;
  script = v8_compile("var bar = 42; other.eval('bar');");
  Local<Value> result = script->Run();
  CHECK(try_catch.HasCaught());
  try_catch.Reset();

  // Check that local variables in current context are not visible in other
  // context.
  script = v8_compile(
      "(function() { "
      "  var baz = 87;"
      "  return other.eval('baz');"
      "})();");
  result = script->Run();
  CHECK(try_catch.HasCaught());
  try_catch.Reset();

  // Check that global variables in the other environment are visible
  // when evaluting code.
  other->Global()->Set(v8_str("bis"), v8_num(1234));
  script = v8_compile("other.eval('bis')");
  CHECK_EQ(1234, script->Run()->Int32Value());
  CHECK(!try_catch.HasCaught());

  // Check that the 'this' pointer points to the global object evaluating
  // code.
  other->Global()->Set(v8_str("t"), other->Global());
  script = v8_compile("other.eval('this == t')");
  result = script->Run();
  CHECK(result->IsTrue());
  CHECK(!try_catch.HasCaught());

  // Check that variables introduced in with-statement are not visible in
  // other context.
  script = v8_compile("with({x:2}){other.eval('x')}");
  result = script->Run();
  CHECK(try_catch.HasCaught());
  try_catch.Reset();

  // Check that you cannot use 'eval.call' with another object than the
  // current global object.
  script = v8_compile("other.y = 1; eval.call(other, 'y')");
  result = script->Run();
  CHECK(try_catch.HasCaught());
}


// Test that calling eval in a context which has been detached from
// its global proxy works.
THREADED_TEST(EvalInDetachedGlobal) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);

  v8::Local<Context> context0 = Context::New(isolate);
  v8::Local<Context> context1 = Context::New(isolate);

  // Set up function in context0 that uses eval from context0.
  context0->Enter();
  v8::Handle<v8::Value> fun = CompileRun(
      "var x = 42;"
      "(function() {"
      "  var e = eval;"
      "  return function(s) { return e(s); }"
      "})()");
  context0->Exit();

  // Put the function into context1 and call it before and after
  // detaching the global.  Before detaching, the call succeeds and
  // after detaching and exception is thrown.
  context1->Enter();
  context1->Global()->Set(v8_str("fun"), fun);
  v8::Handle<v8::Value> x_value = CompileRun("fun('x')");
  CHECK_EQ(42, x_value->Int32Value());
  context0->DetachGlobal();
  v8::TryCatch catcher;
  x_value = CompileRun("fun('x')");
  CHECK_EQ(42, x_value->Int32Value());
  context1->Exit();
}


THREADED_TEST(CrossLazyLoad) {
  v8::HandleScope scope(CcTest::isolate());
  LocalContext other;
  LocalContext current;

  Local<String> token = v8_str("<security token>");
  other->SetSecurityToken(token);
  current->SetSecurityToken(token);

  // Set up reference from current to other.
  current->Global()->Set(v8_str("other"), other->Global());

  // Trigger lazy loading in other context.
  Local<Script> script = v8_compile("other.eval('new Date(42)')");
  Local<Value> value = script->Run();
  CHECK_EQ(42.0, value->NumberValue());
}


static void call_as_function(const v8::FunctionCallbackInfo<v8::Value>& args) {
  ApiTestFuzzer::Fuzz();
  if (args.IsConstructCall()) {
    if (args[0]->IsInt32()) {
      args.GetReturnValue().Set(v8_num(-args[0]->Int32Value()));
      return;
    }
  }

  args.GetReturnValue().Set(args[0]);
}


static void ReturnThis(const v8::FunctionCallbackInfo<v8::Value>& args) {
  args.GetReturnValue().Set(args.This());
}


// Test that a call handler can be set for objects which will allow
// non-function objects created through the API to be called as
// functions.
THREADED_TEST(CallAsFunction) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);

  {
    Local<v8::FunctionTemplate> t = v8::FunctionTemplate::New(isolate);
    Local<ObjectTemplate> instance_template = t->InstanceTemplate();
    instance_template->SetCallAsFunctionHandler(call_as_function);
    Local<v8::Object> instance = t->GetFunction()->NewInstance();
    context->Global()->Set(v8_str("obj"), instance);
    v8::TryCatch try_catch;
    Local<Value> value;
    CHECK(!try_catch.HasCaught());

    value = CompileRun("obj(42)");
    CHECK(!try_catch.HasCaught());
    CHECK_EQ(42, value->Int32Value());

    value = CompileRun("(function(o){return o(49)})(obj)");
    CHECK(!try_catch.HasCaught());
    CHECK_EQ(49, value->Int32Value());

    // test special case of call as function
    value = CompileRun("[obj]['0'](45)");
    CHECK(!try_catch.HasCaught());
    CHECK_EQ(45, value->Int32Value());

    value = CompileRun(
        "obj.call = Function.prototype.call;"
        "obj.call(null, 87)");
    CHECK(!try_catch.HasCaught());
    CHECK_EQ(87, value->Int32Value());

    // Regression tests for bug #1116356: Calling call through call/apply
    // must work for non-function receivers.
    const char* apply_99 = "Function.prototype.call.apply(obj, [this, 99])";
    value = CompileRun(apply_99);
    CHECK(!try_catch.HasCaught());
    CHECK_EQ(99, value->Int32Value());

    const char* call_17 = "Function.prototype.call.call(obj, this, 17)";
    value = CompileRun(call_17);
    CHECK(!try_catch.HasCaught());
    CHECK_EQ(17, value->Int32Value());

    // Check that the call-as-function handler can be called through
    // new.
    value = CompileRun("new obj(43)");
    CHECK(!try_catch.HasCaught());
    CHECK_EQ(-43, value->Int32Value());

    // Check that the call-as-function handler can be called through
    // the API.
    v8::Handle<Value> args[] = {v8_num(28)};
    value = instance->CallAsFunction(instance, 1, args);
    CHECK(!try_catch.HasCaught());
    CHECK_EQ(28, value->Int32Value());
  }

  {
    Local<v8::FunctionTemplate> t = v8::FunctionTemplate::New(isolate);
    Local<ObjectTemplate> instance_template(t->InstanceTemplate());
    USE(instance_template);
    Local<v8::Object> instance = t->GetFunction()->NewInstance();
    context->Global()->Set(v8_str("obj2"), instance);
    v8::TryCatch try_catch;
    Local<Value> value;
    CHECK(!try_catch.HasCaught());

    // Call an object without call-as-function handler through the JS
    value = CompileRun("obj2(28)");
    CHECK(value.IsEmpty());
    CHECK(try_catch.HasCaught());
    String::Utf8Value exception_value1(try_catch.Exception());
    // TODO(verwaest): Better message
    CHECK_EQ(0, strcmp("TypeError: obj2 is not a function", *exception_value1));
    try_catch.Reset();

    // Call an object without call-as-function handler through the API
    value = CompileRun("obj2(28)");
    v8::Handle<Value> args[] = {v8_num(28)};
    value = instance->CallAsFunction(instance, 1, args);
    CHECK(value.IsEmpty());
    CHECK(try_catch.HasCaught());
    String::Utf8Value exception_value2(try_catch.Exception());
    CHECK_EQ(0, strcmp("TypeError: [object Object] is not a function",
                       *exception_value2));
    try_catch.Reset();
  }

  {
    Local<v8::FunctionTemplate> t = v8::FunctionTemplate::New(isolate);
    Local<ObjectTemplate> instance_template = t->InstanceTemplate();
    instance_template->SetCallAsFunctionHandler(ThrowValue);
    Local<v8::Object> instance = t->GetFunction()->NewInstance();
    context->Global()->Set(v8_str("obj3"), instance);
    v8::TryCatch try_catch;
    Local<Value> value;
    CHECK(!try_catch.HasCaught());

    // Catch the exception which is thrown by call-as-function handler
    value = CompileRun("obj3(22)");
    CHECK(try_catch.HasCaught());
    String::Utf8Value exception_value1(try_catch.Exception());
    CHECK_EQ(0, strcmp("22", *exception_value1));
    try_catch.Reset();

    v8::Handle<Value> args[] = {v8_num(23)};
    value = instance->CallAsFunction(instance, 1, args);
    CHECK(try_catch.HasCaught());
    String::Utf8Value exception_value2(try_catch.Exception());
    CHECK_EQ(0, strcmp("23", *exception_value2));
    try_catch.Reset();
  }

  {
    Local<v8::FunctionTemplate> t = v8::FunctionTemplate::New(isolate);
    Local<ObjectTemplate> instance_template = t->InstanceTemplate();
    instance_template->SetCallAsFunctionHandler(ReturnThis);
    Local<v8::Object> instance = t->GetFunction()->NewInstance();

    Local<v8::Value> a1 =
        instance->CallAsFunction(v8::Undefined(isolate), 0, NULL);
    CHECK(a1->StrictEquals(instance));
    Local<v8::Value> a2 = instance->CallAsFunction(v8::Null(isolate), 0, NULL);
    CHECK(a2->StrictEquals(instance));
    Local<v8::Value> a3 = instance->CallAsFunction(v8_num(42), 0, NULL);
    CHECK(a3->StrictEquals(instance));
    Local<v8::Value> a4 = instance->CallAsFunction(v8_str("hello"), 0, NULL);
    CHECK(a4->StrictEquals(instance));
    Local<v8::Value> a5 = instance->CallAsFunction(v8::True(isolate), 0, NULL);
    CHECK(a5->StrictEquals(instance));
  }

  {
    CompileRun(
        "function ReturnThisSloppy() {"
        "  return this;"
        "}"
        "function ReturnThisStrict() {"
        "  'use strict';"
        "  return this;"
        "}");
    Local<Function> ReturnThisSloppy = Local<Function>::Cast(
        context->Global()->Get(v8_str("ReturnThisSloppy")));
    Local<Function> ReturnThisStrict = Local<Function>::Cast(
        context->Global()->Get(v8_str("ReturnThisStrict")));

    Local<v8::Value> a1 =
        ReturnThisSloppy->CallAsFunction(v8::Undefined(isolate), 0, NULL);
    CHECK(a1->StrictEquals(context->Global()));
    Local<v8::Value> a2 =
        ReturnThisSloppy->CallAsFunction(v8::Null(isolate), 0, NULL);
    CHECK(a2->StrictEquals(context->Global()));
    Local<v8::Value> a3 = ReturnThisSloppy->CallAsFunction(v8_num(42), 0, NULL);
    CHECK(a3->IsNumberObject());
    CHECK_EQ(42.0, a3.As<v8::NumberObject>()->ValueOf());
    Local<v8::Value> a4 =
        ReturnThisSloppy->CallAsFunction(v8_str("hello"), 0, NULL);
    CHECK(a4->IsStringObject());
    CHECK(a4.As<v8::StringObject>()->ValueOf()->StrictEquals(v8_str("hello")));
    Local<v8::Value> a5 =
        ReturnThisSloppy->CallAsFunction(v8::True(isolate), 0, NULL);
    CHECK(a5->IsBooleanObject());
    CHECK(a5.As<v8::BooleanObject>()->ValueOf());

    Local<v8::Value> a6 =
        ReturnThisStrict->CallAsFunction(v8::Undefined(isolate), 0, NULL);
    CHECK(a6->IsUndefined());
    Local<v8::Value> a7 =
        ReturnThisStrict->CallAsFunction(v8::Null(isolate), 0, NULL);
    CHECK(a7->IsNull());
    Local<v8::Value> a8 = ReturnThisStrict->CallAsFunction(v8_num(42), 0, NULL);
    CHECK(a8->StrictEquals(v8_num(42)));
    Local<v8::Value> a9 =
        ReturnThisStrict->CallAsFunction(v8_str("hello"), 0, NULL);
    CHECK(a9->StrictEquals(v8_str("hello")));
    Local<v8::Value> a10 =
        ReturnThisStrict->CallAsFunction(v8::True(isolate), 0, NULL);
    CHECK(a10->StrictEquals(v8::True(isolate)));
  }
}


// Check whether a non-function object is callable.
THREADED_TEST(CallableObject) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);

  {
    Local<ObjectTemplate> instance_template = ObjectTemplate::New(isolate);
    instance_template->SetCallAsFunctionHandler(call_as_function);
    Local<Object> instance = instance_template->NewInstance();
    v8::TryCatch try_catch;

    CHECK(instance->IsCallable());
    CHECK(!try_catch.HasCaught());
  }

  {
    Local<ObjectTemplate> instance_template = ObjectTemplate::New(isolate);
    Local<Object> instance = instance_template->NewInstance();
    v8::TryCatch try_catch;

    CHECK(!instance->IsCallable());
    CHECK(!try_catch.HasCaught());
  }

  {
    Local<FunctionTemplate> function_template =
        FunctionTemplate::New(isolate, call_as_function);
    Local<Function> function = function_template->GetFunction();
    Local<Object> instance = function;
    v8::TryCatch try_catch;

    CHECK(instance->IsCallable());
    CHECK(!try_catch.HasCaught());
  }

  {
    Local<FunctionTemplate> function_template = FunctionTemplate::New(isolate);
    Local<Function> function = function_template->GetFunction();
    Local<Object> instance = function;
    v8::TryCatch try_catch;

    CHECK(instance->IsCallable());
    CHECK(!try_catch.HasCaught());
  }
}


static int Recurse(v8::Isolate* isolate, int depth, int iterations) {
  v8::HandleScope scope(isolate);
  if (depth == 0) return v8::HandleScope::NumberOfHandles(isolate);
  for (int i = 0; i < iterations; i++) {
    Local<v8::Number> n(v8::Integer::New(isolate, 42));
  }
  return Recurse(isolate, depth - 1, iterations);
}


THREADED_TEST(HandleIteration) {
  static const int kIterations = 500;
  static const int kNesting = 200;
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope0(isolate);
  CHECK_EQ(0, v8::HandleScope::NumberOfHandles(isolate));
  {
    v8::HandleScope scope1(isolate);
    CHECK_EQ(0, v8::HandleScope::NumberOfHandles(isolate));
    for (int i = 0; i < kIterations; i++) {
      Local<v8::Number> n(v8::Integer::New(CcTest::isolate(), 42));
      CHECK_EQ(i + 1, v8::HandleScope::NumberOfHandles(isolate));
    }

    CHECK_EQ(kIterations, v8::HandleScope::NumberOfHandles(isolate));
    {
      v8::HandleScope scope2(CcTest::isolate());
      for (int j = 0; j < kIterations; j++) {
        Local<v8::Number> n(v8::Integer::New(CcTest::isolate(), 42));
        CHECK_EQ(j + 1 + kIterations,
                 v8::HandleScope::NumberOfHandles(isolate));
      }
    }
    CHECK_EQ(kIterations, v8::HandleScope::NumberOfHandles(isolate));
  }
  CHECK_EQ(0, v8::HandleScope::NumberOfHandles(isolate));
  CHECK_EQ(kNesting * kIterations, Recurse(isolate, kNesting, kIterations));
}


static void InterceptorCallICFastApi(
    Local<Name> name, const v8::PropertyCallbackInfo<v8::Value>& info) {
  ApiTestFuzzer::Fuzz();
  CheckReturnValue(info, FUNCTION_ADDR(InterceptorCallICFastApi));
  int* call_count =
      reinterpret_cast<int*>(v8::External::Cast(*info.Data())->Value());
  ++(*call_count);
  if ((*call_count) % 20 == 0) {
    CcTest::heap()->CollectAllGarbage(i::Heap::kNoGCFlags);
  }
}

static void FastApiCallback_TrivialSignature(
    const v8::FunctionCallbackInfo<v8::Value>& args) {
  ApiTestFuzzer::Fuzz();
  CheckReturnValue(args, FUNCTION_ADDR(FastApiCallback_TrivialSignature));
  v8::Isolate* isolate = CcTest::isolate();
  CHECK_EQ(isolate, args.GetIsolate());
  CHECK(args.This()->Equals(args.Holder()));
  CHECK(args.Data()->Equals(v8_str("method_data")));
  args.GetReturnValue().Set(args[0]->Int32Value() + 1);
}

static void FastApiCallback_SimpleSignature(
    const v8::FunctionCallbackInfo<v8::Value>& args) {
  ApiTestFuzzer::Fuzz();
  CheckReturnValue(args, FUNCTION_ADDR(FastApiCallback_SimpleSignature));
  v8::Isolate* isolate = CcTest::isolate();
  CHECK_EQ(isolate, args.GetIsolate());
  CHECK(args.This()->GetPrototype()->Equals(args.Holder()));
  CHECK(args.Data()->Equals(v8_str("method_data")));
  // Note, we're using HasRealNamedProperty instead of Has to avoid
  // invoking the interceptor again.
  CHECK(args.Holder()->HasRealNamedProperty(v8_str("foo")));
  args.GetReturnValue().Set(args[0]->Int32Value() + 1);
}


// Helper to maximize the odds of object moving.
static void GenerateSomeGarbage() {
  CompileRun(
      "var garbage;"
      "for (var i = 0; i < 1000; i++) {"
      "  garbage = [1/i, \"garbage\" + i, garbage, {foo: garbage}];"
      "}"
      "garbage = undefined;");
}


void DirectApiCallback(const v8::FunctionCallbackInfo<v8::Value>& args) {
  static int count = 0;
  if (count++ % 3 == 0) {
    CcTest::heap()->CollectAllGarbage(i::Heap::kAbortIncrementalMarkingMask);
        // This should move the stub
    GenerateSomeGarbage();  // This should ensure the old stub memory is flushed
  }
}


THREADED_TEST(CallICFastApi_DirectCall_GCMoveStub) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);
  v8::Handle<v8::ObjectTemplate> nativeobject_templ =
      v8::ObjectTemplate::New(isolate);
  nativeobject_templ->Set(isolate, "callback",
                          v8::FunctionTemplate::New(isolate,
                                                    DirectApiCallback));
  v8::Local<v8::Object> nativeobject_obj = nativeobject_templ->NewInstance();
  context->Global()->Set(v8_str("nativeobject"), nativeobject_obj);
  // call the api function multiple times to ensure direct call stub creation.
  CompileRun(
        "function f() {"
        "  for (var i = 1; i <= 30; i++) {"
        "    nativeobject.callback();"
        "  }"
        "}"
        "f();");
}


void ThrowingDirectApiCallback(
    const v8::FunctionCallbackInfo<v8::Value>& args) {
  args.GetIsolate()->ThrowException(v8_str("g"));
}


THREADED_TEST(CallICFastApi_DirectCall_Throw) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);
  v8::Handle<v8::ObjectTemplate> nativeobject_templ =
      v8::ObjectTemplate::New(isolate);
  nativeobject_templ->Set(isolate, "callback",
                          v8::FunctionTemplate::New(isolate,
                                                    ThrowingDirectApiCallback));
  v8::Local<v8::Object> nativeobject_obj = nativeobject_templ->NewInstance();
  context->Global()->Set(v8_str("nativeobject"), nativeobject_obj);
  // call the api function multiple times to ensure direct call stub creation.
  v8::Handle<Value> result = CompileRun(
      "var result = '';"
      "function f() {"
      "  for (var i = 1; i <= 5; i++) {"
      "    try { nativeobject.callback(); } catch (e) { result += e; }"
      "  }"
      "}"
      "f(); result;");
  CHECK(v8_str("ggggg")->Equals(result));
}


static int p_getter_count_3;


static Handle<Value> DoDirectGetter() {
  if (++p_getter_count_3 % 3 == 0) {
    CcTest::heap()->CollectAllGarbage(i::Heap::kAbortIncrementalMarkingMask);
    GenerateSomeGarbage();
  }
  return v8_str("Direct Getter Result");
}


static void DirectGetterCallback(
    Local<String> name,
    const v8::PropertyCallbackInfo<v8::Value>& info) {
  CheckReturnValue(info, FUNCTION_ADDR(DirectGetterCallback));
  info.GetReturnValue().Set(DoDirectGetter());
}


template<typename Accessor>
static void LoadICFastApi_DirectCall_GCMoveStub(Accessor accessor) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);
  v8::Handle<v8::ObjectTemplate> obj = v8::ObjectTemplate::New(isolate);
  obj->SetAccessor(v8_str("p1"), accessor);
  context->Global()->Set(v8_str("o1"), obj->NewInstance());
  p_getter_count_3 = 0;
  v8::Handle<v8::Value> result = CompileRun(
      "function f() {"
      "  for (var i = 0; i < 30; i++) o1.p1;"
      "  return o1.p1"
      "}"
      "f();");
  CHECK(v8_str("Direct Getter Result")->Equals(result));
  CHECK_EQ(31, p_getter_count_3);
}


THREADED_PROFILED_TEST(LoadICFastApi_DirectCall_GCMoveStub) {
  LoadICFastApi_DirectCall_GCMoveStub(DirectGetterCallback);
}


void ThrowingDirectGetterCallback(
    Local<String> name,
    const v8::PropertyCallbackInfo<v8::Value>& info) {
  info.GetIsolate()->ThrowException(v8_str("g"));
}


THREADED_TEST(LoadICFastApi_DirectCall_Throw) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);
  v8::Handle<v8::ObjectTemplate> obj = v8::ObjectTemplate::New(isolate);
  obj->SetAccessor(v8_str("p1"), ThrowingDirectGetterCallback);
  context->Global()->Set(v8_str("o1"), obj->NewInstance());
  v8::Handle<Value> result = CompileRun(
      "var result = '';"
      "for (var i = 0; i < 5; i++) {"
      "    try { o1.p1; } catch (e) { result += e; }"
      "}"
      "result;");
  CHECK(v8_str("ggggg")->Equals(result));
}


THREADED_PROFILED_TEST(InterceptorCallICFastApi_TrivialSignature) {
  int interceptor_call_count = 0;
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  v8::Handle<v8::FunctionTemplate> fun_templ =
      v8::FunctionTemplate::New(isolate);
  v8::Handle<v8::FunctionTemplate> method_templ =
      v8::FunctionTemplate::New(isolate,
                                FastApiCallback_TrivialSignature,
                                v8_str("method_data"),
                                v8::Handle<v8::Signature>());
  v8::Handle<v8::ObjectTemplate> proto_templ = fun_templ->PrototypeTemplate();
  proto_templ->Set(v8_str("method"), method_templ);
  v8::Handle<v8::ObjectTemplate> templ = fun_templ->InstanceTemplate();
  templ->SetHandler(v8::NamedPropertyHandlerConfiguration(
      InterceptorCallICFastApi, NULL, NULL, NULL, NULL,
      v8::External::New(isolate, &interceptor_call_count)));
  LocalContext context;
  v8::Handle<v8::Function> fun = fun_templ->GetFunction();
  GenerateSomeGarbage();
  context->Global()->Set(v8_str("o"), fun->NewInstance());
  CompileRun(
      "var result = 0;"
      "for (var i = 0; i < 100; i++) {"
      "  result = o.method(41);"
      "}");
  CHECK_EQ(42, context->Global()->Get(v8_str("result"))->Int32Value());
  CHECK_EQ(100, interceptor_call_count);
}


THREADED_PROFILED_TEST(InterceptorCallICFastApi_SimpleSignature) {
  int interceptor_call_count = 0;
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  v8::Handle<v8::FunctionTemplate> fun_templ =
      v8::FunctionTemplate::New(isolate);
  v8::Handle<v8::FunctionTemplate> method_templ = v8::FunctionTemplate::New(
      isolate, FastApiCallback_SimpleSignature, v8_str("method_data"),
      v8::Signature::New(isolate, fun_templ));
  v8::Handle<v8::ObjectTemplate> proto_templ = fun_templ->PrototypeTemplate();
  proto_templ->Set(v8_str("method"), method_templ);
  fun_templ->SetHiddenPrototype(true);
  v8::Handle<v8::ObjectTemplate> templ = fun_templ->InstanceTemplate();
  templ->SetHandler(v8::NamedPropertyHandlerConfiguration(
      InterceptorCallICFastApi, NULL, NULL, NULL, NULL,
      v8::External::New(isolate, &interceptor_call_count)));
  LocalContext context;
  v8::Handle<v8::Function> fun = fun_templ->GetFunction();
  GenerateSomeGarbage();
  context->Global()->Set(v8_str("o"), fun->NewInstance());
  CompileRun(
      "o.foo = 17;"
      "var receiver = {};"
      "receiver.__proto__ = o;"
      "var result = 0;"
      "for (var i = 0; i < 100; i++) {"
      "  result = receiver.method(41);"
      "}");
  CHECK_EQ(42, context->Global()->Get(v8_str("result"))->Int32Value());
  CHECK_EQ(100, interceptor_call_count);
}


THREADED_PROFILED_TEST(InterceptorCallICFastApi_SimpleSignature_Miss1) {
  int interceptor_call_count = 0;
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  v8::Handle<v8::FunctionTemplate> fun_templ =
      v8::FunctionTemplate::New(isolate);
  v8::Handle<v8::FunctionTemplate> method_templ = v8::FunctionTemplate::New(
      isolate, FastApiCallback_SimpleSignature, v8_str("method_data"),
      v8::Signature::New(isolate, fun_templ));
  v8::Handle<v8::ObjectTemplate> proto_templ = fun_templ->PrototypeTemplate();
  proto_templ->Set(v8_str("method"), method_templ);
  fun_templ->SetHiddenPrototype(true);
  v8::Handle<v8::ObjectTemplate> templ = fun_templ->InstanceTemplate();
  templ->SetHandler(v8::NamedPropertyHandlerConfiguration(
      InterceptorCallICFastApi, NULL, NULL, NULL, NULL,
      v8::External::New(isolate, &interceptor_call_count)));
  LocalContext context;
  v8::Handle<v8::Function> fun = fun_templ->GetFunction();
  GenerateSomeGarbage();
  context->Global()->Set(v8_str("o"), fun->NewInstance());
  CompileRun(
      "o.foo = 17;"
      "var receiver = {};"
      "receiver.__proto__ = o;"
      "var result = 0;"
      "var saved_result = 0;"
      "for (var i = 0; i < 100; i++) {"
      "  result = receiver.method(41);"
      "  if (i == 50) {"
      "    saved_result = result;"
      "    receiver = {method: function(x) { return x - 1 }};"
      "  }"
      "}");
  CHECK_EQ(40, context->Global()->Get(v8_str("result"))->Int32Value());
  CHECK_EQ(42, context->Global()->Get(v8_str("saved_result"))->Int32Value());
  CHECK_GE(interceptor_call_count, 50);
}


THREADED_PROFILED_TEST(InterceptorCallICFastApi_SimpleSignature_Miss2) {
  int interceptor_call_count = 0;
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  v8::Handle<v8::FunctionTemplate> fun_templ =
      v8::FunctionTemplate::New(isolate);
  v8::Handle<v8::FunctionTemplate> method_templ = v8::FunctionTemplate::New(
      isolate, FastApiCallback_SimpleSignature, v8_str("method_data"),
      v8::Signature::New(isolate, fun_templ));
  v8::Handle<v8::ObjectTemplate> proto_templ = fun_templ->PrototypeTemplate();
  proto_templ->Set(v8_str("method"), method_templ);
  fun_templ->SetHiddenPrototype(true);
  v8::Handle<v8::ObjectTemplate> templ = fun_templ->InstanceTemplate();
  templ->SetHandler(v8::NamedPropertyHandlerConfiguration(
      InterceptorCallICFastApi, NULL, NULL, NULL, NULL,
      v8::External::New(isolate, &interceptor_call_count)));
  LocalContext context;
  v8::Handle<v8::Function> fun = fun_templ->GetFunction();
  GenerateSomeGarbage();
  context->Global()->Set(v8_str("o"), fun->NewInstance());
  CompileRun(
      "o.foo = 17;"
      "var receiver = {};"
      "receiver.__proto__ = o;"
      "var result = 0;"
      "var saved_result = 0;"
      "for (var i = 0; i < 100; i++) {"
      "  result = receiver.method(41);"
      "  if (i == 50) {"
      "    saved_result = result;"
      "    o.method = function(x) { return x - 1 };"
      "  }"
      "}");
  CHECK_EQ(40, context->Global()->Get(v8_str("result"))->Int32Value());
  CHECK_EQ(42, context->Global()->Get(v8_str("saved_result"))->Int32Value());
  CHECK_GE(interceptor_call_count, 50);
}


THREADED_PROFILED_TEST(InterceptorCallICFastApi_SimpleSignature_Miss3) {
  int interceptor_call_count = 0;
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  v8::Handle<v8::FunctionTemplate> fun_templ =
      v8::FunctionTemplate::New(isolate);
  v8::Handle<v8::FunctionTemplate> method_templ = v8::FunctionTemplate::New(
      isolate, FastApiCallback_SimpleSignature, v8_str("method_data"),
      v8::Signature::New(isolate, fun_templ));
  v8::Handle<v8::ObjectTemplate> proto_templ = fun_templ->PrototypeTemplate();
  proto_templ->Set(v8_str("method"), method_templ);
  fun_templ->SetHiddenPrototype(true);
  v8::Handle<v8::ObjectTemplate> templ = fun_templ->InstanceTemplate();
  templ->SetHandler(v8::NamedPropertyHandlerConfiguration(
      InterceptorCallICFastApi, NULL, NULL, NULL, NULL,
      v8::External::New(isolate, &interceptor_call_count)));
  LocalContext context;
  v8::Handle<v8::Function> fun = fun_templ->GetFunction();
  GenerateSomeGarbage();
  context->Global()->Set(v8_str("o"), fun->NewInstance());
  v8::TryCatch try_catch;
  CompileRun(
      "o.foo = 17;"
      "var receiver = {};"
      "receiver.__proto__ = o;"
      "var result = 0;"
      "var saved_result = 0;"
      "for (var i = 0; i < 100; i++) {"
      "  result = receiver.method(41);"
      "  if (i == 50) {"
      "    saved_result = result;"
      "    receiver = 333;"
      "  }"
      "}");
  CHECK(try_catch.HasCaught());
  // TODO(verwaest): Adjust message.
  CHECK(v8_str("TypeError: receiver.method is not a function")
            ->Equals(try_catch.Exception()->ToString(isolate)));
  CHECK_EQ(42, context->Global()->Get(v8_str("saved_result"))->Int32Value());
  CHECK_GE(interceptor_call_count, 50);
}


THREADED_PROFILED_TEST(InterceptorCallICFastApi_SimpleSignature_TypeError) {
  int interceptor_call_count = 0;
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  v8::Handle<v8::FunctionTemplate> fun_templ =
      v8::FunctionTemplate::New(isolate);
  v8::Handle<v8::FunctionTemplate> method_templ = v8::FunctionTemplate::New(
      isolate, FastApiCallback_SimpleSignature, v8_str("method_data"),
      v8::Signature::New(isolate, fun_templ));
  v8::Handle<v8::ObjectTemplate> proto_templ = fun_templ->PrototypeTemplate();
  proto_templ->Set(v8_str("method"), method_templ);
  fun_templ->SetHiddenPrototype(true);
  v8::Handle<v8::ObjectTemplate> templ = fun_templ->InstanceTemplate();
  templ->SetHandler(v8::NamedPropertyHandlerConfiguration(
      InterceptorCallICFastApi, NULL, NULL, NULL, NULL,
      v8::External::New(isolate, &interceptor_call_count)));
  LocalContext context;
  v8::Handle<v8::Function> fun = fun_templ->GetFunction();
  GenerateSomeGarbage();
  context->Global()->Set(v8_str("o"), fun->NewInstance());
  v8::TryCatch try_catch;
  CompileRun(
      "o.foo = 17;"
      "var receiver = {};"
      "receiver.__proto__ = o;"
      "var result = 0;"
      "var saved_result = 0;"
      "for (var i = 0; i < 100; i++) {"
      "  result = receiver.method(41);"
      "  if (i == 50) {"
      "    saved_result = result;"
      "    receiver = {method: receiver.method};"
      "  }"
      "}");
  CHECK(try_catch.HasCaught());
  CHECK(v8_str("TypeError: Illegal invocation")
            ->Equals(try_catch.Exception()->ToString(isolate)));
  CHECK_EQ(42, context->Global()->Get(v8_str("saved_result"))->Int32Value());
  CHECK_GE(interceptor_call_count, 50);
}


THREADED_PROFILED_TEST(CallICFastApi_TrivialSignature) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  v8::Handle<v8::FunctionTemplate> fun_templ =
      v8::FunctionTemplate::New(isolate);
  v8::Handle<v8::FunctionTemplate> method_templ =
      v8::FunctionTemplate::New(isolate,
                                FastApiCallback_TrivialSignature,
                                v8_str("method_data"),
                                v8::Handle<v8::Signature>());
  v8::Handle<v8::ObjectTemplate> proto_templ = fun_templ->PrototypeTemplate();
  proto_templ->Set(v8_str("method"), method_templ);
  v8::Handle<v8::ObjectTemplate> templ(fun_templ->InstanceTemplate());
  USE(templ);
  LocalContext context;
  v8::Handle<v8::Function> fun = fun_templ->GetFunction();
  GenerateSomeGarbage();
  context->Global()->Set(v8_str("o"), fun->NewInstance());
  CompileRun(
      "var result = 0;"
      "for (var i = 0; i < 100; i++) {"
      "  result = o.method(41);"
      "}");

  CHECK_EQ(42, context->Global()->Get(v8_str("result"))->Int32Value());
}


THREADED_PROFILED_TEST(CallICFastApi_SimpleSignature) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  v8::Handle<v8::FunctionTemplate> fun_templ =
      v8::FunctionTemplate::New(isolate);
  v8::Handle<v8::FunctionTemplate> method_templ = v8::FunctionTemplate::New(
      isolate, FastApiCallback_SimpleSignature, v8_str("method_data"),
      v8::Signature::New(isolate, fun_templ));
  v8::Handle<v8::ObjectTemplate> proto_templ = fun_templ->PrototypeTemplate();
  proto_templ->Set(v8_str("method"), method_templ);
  fun_templ->SetHiddenPrototype(true);
  v8::Handle<v8::ObjectTemplate> templ(fun_templ->InstanceTemplate());
  CHECK(!templ.IsEmpty());
  LocalContext context;
  v8::Handle<v8::Function> fun = fun_templ->GetFunction();
  GenerateSomeGarbage();
  context->Global()->Set(v8_str("o"), fun->NewInstance());
  CompileRun(
      "o.foo = 17;"
      "var receiver = {};"
      "receiver.__proto__ = o;"
      "var result = 0;"
      "for (var i = 0; i < 100; i++) {"
      "  result = receiver.method(41);"
      "}");

  CHECK_EQ(42, context->Global()->Get(v8_str("result"))->Int32Value());
}


THREADED_PROFILED_TEST(CallICFastApi_SimpleSignature_Miss1) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  v8::Handle<v8::FunctionTemplate> fun_templ =
      v8::FunctionTemplate::New(isolate);
  v8::Handle<v8::FunctionTemplate> method_templ = v8::FunctionTemplate::New(
      isolate, FastApiCallback_SimpleSignature, v8_str("method_data"),
      v8::Signature::New(isolate, fun_templ));
  v8::Handle<v8::ObjectTemplate> proto_templ = fun_templ->PrototypeTemplate();
  proto_templ->Set(v8_str("method"), method_templ);
  fun_templ->SetHiddenPrototype(true);
  v8::Handle<v8::ObjectTemplate> templ(fun_templ->InstanceTemplate());
  CHECK(!templ.IsEmpty());
  LocalContext context;
  v8::Handle<v8::Function> fun = fun_templ->GetFunction();
  GenerateSomeGarbage();
  context->Global()->Set(v8_str("o"), fun->NewInstance());
  CompileRun(
      "o.foo = 17;"
      "var receiver = {};"
      "receiver.__proto__ = o;"
      "var result = 0;"
      "var saved_result = 0;"
      "for (var i = 0; i < 100; i++) {"
      "  result = receiver.method(41);"
      "  if (i == 50) {"
      "    saved_result = result;"
      "    receiver = {method: function(x) { return x - 1 }};"
      "  }"
      "}");
  CHECK_EQ(40, context->Global()->Get(v8_str("result"))->Int32Value());
  CHECK_EQ(42, context->Global()->Get(v8_str("saved_result"))->Int32Value());
}


THREADED_PROFILED_TEST(CallICFastApi_SimpleSignature_Miss2) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  v8::Handle<v8::FunctionTemplate> fun_templ =
      v8::FunctionTemplate::New(isolate);
  v8::Handle<v8::FunctionTemplate> method_templ = v8::FunctionTemplate::New(
      isolate, FastApiCallback_SimpleSignature, v8_str("method_data"),
      v8::Signature::New(isolate, fun_templ));
  v8::Handle<v8::ObjectTemplate> proto_templ = fun_templ->PrototypeTemplate();
  proto_templ->Set(v8_str("method"), method_templ);
  fun_templ->SetHiddenPrototype(true);
  v8::Handle<v8::ObjectTemplate> templ(fun_templ->InstanceTemplate());
  CHECK(!templ.IsEmpty());
  LocalContext context;
  v8::Handle<v8::Function> fun = fun_templ->GetFunction();
  GenerateSomeGarbage();
  context->Global()->Set(v8_str("o"), fun->NewInstance());
  v8::TryCatch try_catch;
  CompileRun(
      "o.foo = 17;"
      "var receiver = {};"
      "receiver.__proto__ = o;"
      "var result = 0;"
      "var saved_result = 0;"
      "for (var i = 0; i < 100; i++) {"
      "  result = receiver.method(41);"
      "  if (i == 50) {"
      "    saved_result = result;"
      "    receiver = 333;"
      "  }"
      "}");
  CHECK(try_catch.HasCaught());
  // TODO(verwaest): Adjust message.
  CHECK(v8_str("TypeError: receiver.method is not a function")
            ->Equals(try_catch.Exception()->ToString(isolate)));
  CHECK_EQ(42, context->Global()->Get(v8_str("saved_result"))->Int32Value());
}


THREADED_PROFILED_TEST(CallICFastApi_SimpleSignature_TypeError) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  v8::Handle<v8::FunctionTemplate> fun_templ =
      v8::FunctionTemplate::New(isolate);
  v8::Handle<v8::FunctionTemplate> method_templ = v8::FunctionTemplate::New(
      isolate, FastApiCallback_SimpleSignature, v8_str("method_data"),
      v8::Signature::New(isolate, fun_templ));
  v8::Handle<v8::ObjectTemplate> proto_templ = fun_templ->PrototypeTemplate();
  proto_templ->Set(v8_str("method"), method_templ);
  fun_templ->SetHiddenPrototype(true);
  v8::Handle<v8::ObjectTemplate> templ(fun_templ->InstanceTemplate());
  CHECK(!templ.IsEmpty());
  LocalContext context;
  v8::Handle<v8::Function> fun = fun_templ->GetFunction();
  GenerateSomeGarbage();
  context->Global()->Set(v8_str("o"), fun->NewInstance());
  v8::TryCatch try_catch;
  CompileRun(
      "o.foo = 17;"
      "var receiver = {};"
      "receiver.__proto__ = o;"
      "var result = 0;"
      "var saved_result = 0;"
      "for (var i = 0; i < 100; i++) {"
      "  result = receiver.method(41);"
      "  if (i == 50) {"
      "    saved_result = result;"
      "    receiver = Object.create(receiver);"
      "  }"
      "}");
  CHECK(try_catch.HasCaught());
  CHECK(v8_str("TypeError: Illegal invocation")
            ->Equals(try_catch.Exception()->ToString(isolate)));
  CHECK_EQ(42, context->Global()->Get(v8_str("saved_result"))->Int32Value());
}


static void ThrowingGetter(Local<String> name,
                           const v8::PropertyCallbackInfo<v8::Value>& info) {
  ApiTestFuzzer::Fuzz();
  info.GetIsolate()->ThrowException(Handle<Value>());
  info.GetReturnValue().SetUndefined();
}


THREADED_TEST(VariousGetPropertiesAndThrowingCallbacks) {
  LocalContext context;
  HandleScope scope(context->GetIsolate());

  Local<FunctionTemplate> templ = FunctionTemplate::New(context->GetIsolate());
  Local<ObjectTemplate> instance_templ = templ->InstanceTemplate();
  instance_templ->SetAccessor(v8_str("f"), ThrowingGetter);

  Local<Object> instance = templ->GetFunction()->NewInstance();

  Local<Object> another = Object::New(context->GetIsolate());
  another->SetPrototype(instance);

  Local<Object> with_js_getter = CompileRun(
      "o = {};\n"
      "o.__defineGetter__('f', function() { throw undefined; });\n"
      "o\n").As<Object>();
  CHECK(!with_js_getter.IsEmpty());

  TryCatch try_catch;

  Local<Value> result = instance->GetRealNamedProperty(v8_str("f"));
  CHECK(try_catch.HasCaught());
  try_catch.Reset();
  CHECK(result.IsEmpty());

  Maybe<PropertyAttribute> attr =
      instance->GetRealNamedPropertyAttributes(v8_str("f"));
  CHECK(!try_catch.HasCaught());
  CHECK(Just(None) == attr);

  result = another->GetRealNamedProperty(v8_str("f"));
  CHECK(try_catch.HasCaught());
  try_catch.Reset();
  CHECK(result.IsEmpty());

  attr = another->GetRealNamedPropertyAttributes(v8_str("f"));
  CHECK(!try_catch.HasCaught());
  CHECK(Just(None) == attr);

  result = another->GetRealNamedPropertyInPrototypeChain(v8_str("f"));
  CHECK(try_catch.HasCaught());
  try_catch.Reset();
  CHECK(result.IsEmpty());

  attr = another->GetRealNamedPropertyAttributesInPrototypeChain(v8_str("f"));
  CHECK(!try_catch.HasCaught());
  CHECK(Just(None) == attr);

  result = another->Get(v8_str("f"));
  CHECK(try_catch.HasCaught());
  try_catch.Reset();
  CHECK(result.IsEmpty());

  result = with_js_getter->GetRealNamedProperty(v8_str("f"));
  CHECK(try_catch.HasCaught());
  try_catch.Reset();
  CHECK(result.IsEmpty());

  attr = with_js_getter->GetRealNamedPropertyAttributes(v8_str("f"));
  CHECK(!try_catch.HasCaught());
  CHECK(Just(None) == attr);

  result = with_js_getter->Get(v8_str("f"));
  CHECK(try_catch.HasCaught());
  try_catch.Reset();
  CHECK(result.IsEmpty());
}


static void ThrowingCallbackWithTryCatch(
    const v8::FunctionCallbackInfo<v8::Value>& args) {
  TryCatch try_catch;
  // Verboseness is important: it triggers message delivery which can call into
  // external code.
  try_catch.SetVerbose(true);
  CompileRun("throw 'from JS';");
  CHECK(try_catch.HasCaught());
  CHECK(!CcTest::i_isolate()->has_pending_exception());
  CHECK(!CcTest::i_isolate()->has_scheduled_exception());
}


static int call_depth;


static void WithTryCatch(Handle<Message> message, Handle<Value> data) {
  TryCatch try_catch;
}


static void ThrowFromJS(Handle<Message> message, Handle<Value> data) {
  if (--call_depth) CompileRun("throw 'ThrowInJS';");
}


static void ThrowViaApi(Handle<Message> message, Handle<Value> data) {
  if (--call_depth) CcTest::isolate()->ThrowException(v8_str("ThrowViaApi"));
}


static void WebKitLike(Handle<Message> message, Handle<Value> data) {
  Handle<String> errorMessageString = message->Get();
  CHECK(!errorMessageString.IsEmpty());
  message->GetStackTrace();
  message->GetScriptOrigin().ResourceName();
}


THREADED_TEST(ExceptionsDoNotPropagatePastTryCatch) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  HandleScope scope(isolate);

  Local<Function> func =
      FunctionTemplate::New(isolate,
                            ThrowingCallbackWithTryCatch)->GetFunction();
  context->Global()->Set(v8_str("func"), func);

  MessageCallback callbacks[] =
      { NULL, WebKitLike, ThrowViaApi, ThrowFromJS, WithTryCatch };
  for (unsigned i = 0; i < sizeof(callbacks)/sizeof(callbacks[0]); i++) {
    MessageCallback callback = callbacks[i];
    if (callback != NULL) {
      V8::AddMessageListener(callback);
    }
    // Some small number to control number of times message handler should
    // throw an exception.
    call_depth = 5;
    ExpectFalse(
        "var thrown = false;\n"
        "try { func(); } catch(e) { thrown = true; }\n"
        "thrown\n");
    if (callback != NULL) {
      V8::RemoveMessageListeners(callback);
    }
  }
}


static void ParentGetter(Local<String> name,
                         const v8::PropertyCallbackInfo<v8::Value>& info) {
  ApiTestFuzzer::Fuzz();
  info.GetReturnValue().Set(v8_num(1));
}


static void ChildGetter(Local<String> name,
                        const v8::PropertyCallbackInfo<v8::Value>& info) {
  ApiTestFuzzer::Fuzz();
  info.GetReturnValue().Set(v8_num(42));
}


THREADED_TEST(Overriding) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);

  // Parent template.
  Local<v8::FunctionTemplate> parent_templ = v8::FunctionTemplate::New(isolate);
  Local<ObjectTemplate> parent_instance_templ =
      parent_templ->InstanceTemplate();
  parent_instance_templ->SetAccessor(v8_str("f"), ParentGetter);

  // Template that inherits from the parent template.
  Local<v8::FunctionTemplate> child_templ = v8::FunctionTemplate::New(isolate);
  Local<ObjectTemplate> child_instance_templ =
      child_templ->InstanceTemplate();
  child_templ->Inherit(parent_templ);
  // Override 'f'.  The child version of 'f' should get called for child
  // instances.
  child_instance_templ->SetAccessor(v8_str("f"), ChildGetter);
  // Add 'g' twice.  The 'g' added last should get called for instances.
  child_instance_templ->SetAccessor(v8_str("g"), ParentGetter);
  child_instance_templ->SetAccessor(v8_str("g"), ChildGetter);

  // Add 'h' as an accessor to the proto template with ReadOnly attributes
  // so 'h' can be shadowed on the instance object.
  Local<ObjectTemplate> child_proto_templ = child_templ->PrototypeTemplate();
  child_proto_templ->SetAccessor(v8_str("h"), ParentGetter, 0,
      v8::Handle<Value>(), v8::DEFAULT, v8::ReadOnly);

  // Add 'i' as an accessor to the instance template with ReadOnly attributes
  // but the attribute does not have effect because it is duplicated with
  // NULL setter.
  child_instance_templ->SetAccessor(v8_str("i"), ChildGetter, 0,
      v8::Handle<Value>(), v8::DEFAULT, v8::ReadOnly);



  // Instantiate the child template.
  Local<v8::Object> instance = child_templ->GetFunction()->NewInstance();

  // Check that the child function overrides the parent one.
  context->Global()->Set(v8_str("o"), instance);
  Local<Value> value = v8_compile("o.f")->Run();
  // Check that the 'g' that was added last is hit.
  CHECK_EQ(42, value->Int32Value());
  value = v8_compile("o.g")->Run();
  CHECK_EQ(42, value->Int32Value());

  // Check that 'h' cannot be shadowed.
  value = v8_compile("o.h = 3; o.h")->Run();
  CHECK_EQ(1, value->Int32Value());

  // Check that 'i' cannot be shadowed or changed.
  value = v8_compile("o.i = 3; o.i")->Run();
  CHECK_EQ(42, value->Int32Value());
}


static void IsConstructHandler(
    const v8::FunctionCallbackInfo<v8::Value>& args) {
  ApiTestFuzzer::Fuzz();
  args.GetReturnValue().Set(args.IsConstructCall());
}


THREADED_TEST(IsConstructCall) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);

  // Function template with call handler.
  Local<v8::FunctionTemplate> templ = v8::FunctionTemplate::New(isolate);
  templ->SetCallHandler(IsConstructHandler);

  LocalContext context;

  context->Global()->Set(v8_str("f"), templ->GetFunction());
  Local<Value> value = v8_compile("f()")->Run();
  CHECK(!value->BooleanValue());
  value = v8_compile("new f()")->Run();
  CHECK(value->BooleanValue());
}


THREADED_TEST(ObjectProtoToString) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  Local<v8::FunctionTemplate> templ = v8::FunctionTemplate::New(isolate);
  templ->SetClassName(v8_str("MyClass"));

  LocalContext context;

  Local<String> customized_tostring = v8_str("customized toString");

  // Replace Object.prototype.toString
  v8_compile("Object.prototype.toString = function() {"
                  "  return 'customized toString';"
                  "}")->Run();

  // Normal ToString call should call replaced Object.prototype.toString
  Local<v8::Object> instance = templ->GetFunction()->NewInstance();
  Local<String> value = instance->ToString(isolate);
  CHECK(value->IsString() && value->Equals(customized_tostring));

  // ObjectProtoToString should not call replace toString function.
  value = instance->ObjectProtoToString();
  CHECK(value->IsString() && value->Equals(v8_str("[object MyClass]")));

  // Check global
  value = context->Global()->ObjectProtoToString();
  CHECK(value->IsString() && value->Equals(v8_str("[object global]")));

  // Check ordinary object
  Local<Value> object = v8_compile("new Object()")->Run();
  value = object.As<v8::Object>()->ObjectProtoToString();
  CHECK(value->IsString() && value->Equals(v8_str("[object Object]")));
}


TEST(ObjectProtoToStringES6) {
  // TODO(dslomov, caitp): merge into ObjectProtoToString test once shipped.
  i::FLAG_harmony_tostring = true;
  LocalContext context;
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  Local<v8::FunctionTemplate> templ = v8::FunctionTemplate::New(isolate);
  templ->SetClassName(v8_str("MyClass"));

  Local<String> customized_tostring = v8_str("customized toString");

  // Replace Object.prototype.toString
  CompileRun(
      "Object.prototype.toString = function() {"
      "  return 'customized toString';"
      "}");

  // Normal ToString call should call replaced Object.prototype.toString
  Local<v8::Object> instance = templ->GetFunction()->NewInstance();
  Local<String> value = instance->ToString(isolate);
  CHECK(value->IsString() && value->Equals(customized_tostring));

  // ObjectProtoToString should not call replace toString function.
  value = instance->ObjectProtoToString();
  CHECK(value->IsString() && value->Equals(v8_str("[object MyClass]")));

  // Check global
  value = context->Global()->ObjectProtoToString();
  CHECK(value->IsString() && value->Equals(v8_str("[object global]")));

  // Check ordinary object
  Local<Value> object = CompileRun("new Object()");
  value = object.As<v8::Object>()->ObjectProtoToString();
  CHECK(value->IsString() && value->Equals(v8_str("[object Object]")));

  // Check that ES6 semantics using @@toStringTag work
  Local<v8::Symbol> toStringTag = v8::Symbol::GetToStringTag(isolate);

#define TEST_TOSTRINGTAG(type, tag, expected)                \
  do {                                                       \
    object = CompileRun("new " #type "()");                  \
    object.As<v8::Object>()->Set(toStringTag, v8_str(#tag)); \
    value = object.As<v8::Object>()->ObjectProtoToString();  \
    CHECK(value->IsString() &&                               \
          value->Equals(v8_str("[object " #expected "]")));  \
  } while (0)

  TEST_TOSTRINGTAG(Array, Object, Object);
  TEST_TOSTRINGTAG(Object, Arguments, Arguments);
  TEST_TOSTRINGTAG(Object, Array, Array);
  TEST_TOSTRINGTAG(Object, Boolean, Boolean);
  TEST_TOSTRINGTAG(Object, Date, Date);
  TEST_TOSTRINGTAG(Object, Error, Error);
  TEST_TOSTRINGTAG(Object, Function, Function);
  TEST_TOSTRINGTAG(Object, Number, Number);
  TEST_TOSTRINGTAG(Object, RegExp, RegExp);
  TEST_TOSTRINGTAG(Object, String, String);
  TEST_TOSTRINGTAG(Object, Foo, Foo);

#undef TEST_TOSTRINGTAG

  Local<v8::RegExp> valueRegExp = v8::RegExp::New(v8_str("^$"),
                                                  v8::RegExp::kNone);
  Local<Value> valueNumber = v8_num(123);
  Local<v8::Symbol> valueSymbol = v8_symbol("TestSymbol");
  Local<v8::Function> valueFunction =
      CompileRun("(function fn() {})").As<v8::Function>();
  Local<v8::Object> valueObject = v8::Object::New(v8::Isolate::GetCurrent());
  Local<v8::Primitive> valueNull = v8::Null(v8::Isolate::GetCurrent());
  Local<v8::Primitive> valueUndef = v8::Undefined(v8::Isolate::GetCurrent());

#define TEST_TOSTRINGTAG(type, tagValue, expected)          \
  do {                                                      \
    object = CompileRun("new " #type "()");                 \
    object.As<v8::Object>()->Set(toStringTag, tagValue);    \
    value = object.As<v8::Object>()->ObjectProtoToString(); \
    CHECK(value->IsString() &&                              \
          value->Equals(v8_str("[object " #expected "]"))); \
  } while (0)

#define TEST_TOSTRINGTAG_TYPES(tagValue)                    \
  TEST_TOSTRINGTAG(Array, tagValue, Array);                 \
  TEST_TOSTRINGTAG(Object, tagValue, Object);               \
  TEST_TOSTRINGTAG(Function, tagValue, Function);           \
  TEST_TOSTRINGTAG(Date, tagValue, Date);                   \
  TEST_TOSTRINGTAG(RegExp, tagValue, RegExp);               \
  TEST_TOSTRINGTAG(Error, tagValue, Error);                 \

  // Test non-String-valued @@toStringTag
  TEST_TOSTRINGTAG_TYPES(valueRegExp);
  TEST_TOSTRINGTAG_TYPES(valueNumber);
  TEST_TOSTRINGTAG_TYPES(valueSymbol);
  TEST_TOSTRINGTAG_TYPES(valueFunction);
  TEST_TOSTRINGTAG_TYPES(valueObject);
  TEST_TOSTRINGTAG_TYPES(valueNull);
  TEST_TOSTRINGTAG_TYPES(valueUndef);

#undef TEST_TOSTRINGTAG
#undef TEST_TOSTRINGTAG_TYPES

  // @@toStringTag getter throws
  Local<Value> obj = v8::Object::New(isolate);
  obj.As<v8::Object>()->SetAccessor(toStringTag, ThrowingSymbolAccessorGetter);
  {
    TryCatch try_catch;
    value = obj.As<v8::Object>()->ObjectProtoToString();
    CHECK(value.IsEmpty());
    CHECK(try_catch.HasCaught());
  }

  // @@toStringTag getter does not throw
  obj = v8::Object::New(isolate);
  obj.As<v8::Object>()->SetAccessor(
      toStringTag, SymbolAccessorGetterReturnsDefault, 0, v8_str("Test"));
  {
    TryCatch try_catch;
    value = obj.As<v8::Object>()->ObjectProtoToString();
    CHECK(value->IsString() && value->Equals(v8_str("[object Test]")));
    CHECK(!try_catch.HasCaught());
  }

  // JS @@toStringTag value
  obj = CompileRun("obj = {}; obj[Symbol.toStringTag] = 'Test'; obj");
  {
    TryCatch try_catch;
    value = obj.As<v8::Object>()->ObjectProtoToString();
    CHECK(value->IsString() && value->Equals(v8_str("[object Test]")));
    CHECK(!try_catch.HasCaught());
  }

  // JS @@toStringTag getter throws
  obj = CompileRun(
      "obj = {}; Object.defineProperty(obj, Symbol.toStringTag, {"
      "  get: function() { throw 'Test'; }"
      "}); obj");
  {
    TryCatch try_catch;
    value = obj.As<v8::Object>()->ObjectProtoToString();
    CHECK(value.IsEmpty());
    CHECK(try_catch.HasCaught());
  }

  // JS @@toStringTag getter does not throw
  obj = CompileRun(
      "obj = {}; Object.defineProperty(obj, Symbol.toStringTag, {"
      "  get: function() { return 'Test'; }"
      "}); obj");
  {
    TryCatch try_catch;
    value = obj.As<v8::Object>()->ObjectProtoToString();
    CHECK(value->IsString() && value->Equals(v8_str("[object Test]")));
    CHECK(!try_catch.HasCaught());
  }
}


THREADED_TEST(ObjectGetConstructorName) {
  v8::Isolate* isolate = CcTest::isolate();
  LocalContext context;
  v8::HandleScope scope(isolate);
  v8_compile("function Parent() {};"
             "function Child() {};"
             "Child.prototype = new Parent();"
             "var outer = { inner: function() { } };"
             "var p = new Parent();"
             "var c = new Child();"
             "var x = new outer.inner();")->Run();

  Local<v8::Value> p = context->Global()->Get(v8_str("p"));
  CHECK(p->IsObject() &&
        p->ToObject(isolate)->GetConstructorName()->Equals(v8_str("Parent")));

  Local<v8::Value> c = context->Global()->Get(v8_str("c"));
  CHECK(c->IsObject() &&
        c->ToObject(isolate)->GetConstructorName()->Equals(v8_str("Child")));

  Local<v8::Value> x = context->Global()->Get(v8_str("x"));
  CHECK(x->IsObject() &&
        x->ToObject(isolate)->GetConstructorName()->Equals(
            v8_str("outer.inner")));
}


bool ApiTestFuzzer::fuzzing_ = false;
v8::base::Semaphore ApiTestFuzzer::all_tests_done_(0);
int ApiTestFuzzer::active_tests_;
int ApiTestFuzzer::tests_being_run_;
int ApiTestFuzzer::current_;


// We are in a callback and want to switch to another thread (if we
// are currently running the thread fuzzing test).
void ApiTestFuzzer::Fuzz() {
  if (!fuzzing_) return;
  ApiTestFuzzer* test = RegisterThreadedTest::nth(current_)->fuzzer_;
  test->ContextSwitch();
}


// Let the next thread go.  Since it is also waiting on the V8 lock it may
// not start immediately.
bool ApiTestFuzzer::NextThread() {
  int test_position = GetNextTestNumber();
  const char* test_name = RegisterThreadedTest::nth(current_)->name();
  if (test_position == current_) {
    if (kLogThreading)
      printf("Stay with %s\n", test_name);
    return false;
  }
  if (kLogThreading) {
    printf("Switch from %s to %s\n",
           test_name,
           RegisterThreadedTest::nth(test_position)->name());
  }
  current_ = test_position;
  RegisterThreadedTest::nth(current_)->fuzzer_->gate_.Signal();
  return true;
}


void ApiTestFuzzer::Run() {
  // When it is our turn...
  gate_.Wait();
  {
    // ... get the V8 lock and start running the test.
    v8::Locker locker(CcTest::isolate());
    CallTest();
  }
  // This test finished.
  active_ = false;
  active_tests_--;
  // If it was the last then signal that fact.
  if (active_tests_ == 0) {
    all_tests_done_.Signal();
  } else {
    // Otherwise select a new test and start that.
    NextThread();
  }
}


static unsigned linear_congruential_generator;


void ApiTestFuzzer::SetUp(PartOfTest part) {
  linear_congruential_generator = i::FLAG_testing_prng_seed;
  fuzzing_ = true;
  int count = RegisterThreadedTest::count();
  int start =  count * part / (LAST_PART + 1);
  int end = (count * (part + 1) / (LAST_PART + 1)) - 1;
  active_tests_ = tests_being_run_ = end - start + 1;
  for (int i = 0; i < tests_being_run_; i++) {
    RegisterThreadedTest::nth(i)->fuzzer_ = new ApiTestFuzzer(i + start);
  }
  for (int i = 0; i < active_tests_; i++) {
    RegisterThreadedTest::nth(i)->fuzzer_->Start();
  }
}


static void CallTestNumber(int test_number) {
  (RegisterThreadedTest::nth(test_number)->callback())();
}


void ApiTestFuzzer::RunAllTests() {
  // Set off the first test.
  current_ = -1;
  NextThread();
  // Wait till they are all done.
  all_tests_done_.Wait();
}


int ApiTestFuzzer::GetNextTestNumber() {
  int next_test;
  do {
    next_test = (linear_congruential_generator >> 16) % tests_being_run_;
    linear_congruential_generator *= 1664525u;
    linear_congruential_generator += 1013904223u;
  } while (!RegisterThreadedTest::nth(next_test)->fuzzer_->active_);
  return next_test;
}


void ApiTestFuzzer::ContextSwitch() {
  // If the new thread is the same as the current thread there is nothing to do.
  if (NextThread()) {
    // Now it can start.
    v8::Unlocker unlocker(CcTest::isolate());
    // Wait till someone starts us again.
    gate_.Wait();
    // And we're off.
  }
}


void ApiTestFuzzer::TearDown() {
  fuzzing_ = false;
  for (int i = 0; i < RegisterThreadedTest::count(); i++) {
    ApiTestFuzzer *fuzzer = RegisterThreadedTest::nth(i)->fuzzer_;
    if (fuzzer != NULL) fuzzer->Join();
  }
}


// Lets not be needlessly self-referential.
TEST(Threading1) {
  ApiTestFuzzer::SetUp(ApiTestFuzzer::FIRST_PART);
  ApiTestFuzzer::RunAllTests();
  ApiTestFuzzer::TearDown();
}


TEST(Threading2) {
  ApiTestFuzzer::SetUp(ApiTestFuzzer::SECOND_PART);
  ApiTestFuzzer::RunAllTests();
  ApiTestFuzzer::TearDown();
}


TEST(Threading3) {
  ApiTestFuzzer::SetUp(ApiTestFuzzer::THIRD_PART);
  ApiTestFuzzer::RunAllTests();
  ApiTestFuzzer::TearDown();
}


TEST(Threading4) {
  ApiTestFuzzer::SetUp(ApiTestFuzzer::FOURTH_PART);
  ApiTestFuzzer::RunAllTests();
  ApiTestFuzzer::TearDown();
}


void ApiTestFuzzer::CallTest() {
  v8::Isolate::Scope scope(CcTest::isolate());
  if (kLogThreading)
    printf("Start test %d\n", test_number_);
  CallTestNumber(test_number_);
  if (kLogThreading)
    printf("End test %d\n", test_number_);
}


static void ThrowInJS(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  CHECK(v8::Locker::IsLocked(isolate));
  ApiTestFuzzer::Fuzz();
  v8::Unlocker unlocker(isolate);
  const char* code = "throw 7;";
  {
    v8::Locker nested_locker(isolate);
    v8::HandleScope scope(isolate);
    v8::Handle<Value> exception;
    { v8::TryCatch try_catch;
      v8::Handle<Value> value = CompileRun(code);
      CHECK(value.IsEmpty());
      CHECK(try_catch.HasCaught());
      // Make sure to wrap the exception in a new handle because
      // the handle returned from the TryCatch is destroyed
      // when the TryCatch is destroyed.
      exception = Local<Value>::New(isolate, try_catch.Exception());
    }
    args.GetIsolate()->ThrowException(exception);
  }
}


static void ThrowInJSNoCatch(const v8::FunctionCallbackInfo<v8::Value>& args) {
  CHECK(v8::Locker::IsLocked(CcTest::isolate()));
  ApiTestFuzzer::Fuzz();
  v8::Unlocker unlocker(CcTest::isolate());
  const char* code = "throw 7;";
  {
    v8::Locker nested_locker(CcTest::isolate());
    v8::HandleScope scope(args.GetIsolate());
    v8::Handle<Value> value = CompileRun(code);
    CHECK(value.IsEmpty());
    args.GetReturnValue().Set(v8_str("foo"));
  }
}


// These are locking tests that don't need to be run again
// as part of the locking aggregation tests.
TEST(NestedLockers) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::Locker locker(isolate);
  CHECK(v8::Locker::IsLocked(isolate));
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());
  Local<v8::FunctionTemplate> fun_templ =
      v8::FunctionTemplate::New(isolate, ThrowInJS);
  Local<Function> fun = fun_templ->GetFunction();
  env->Global()->Set(v8_str("throw_in_js"), fun);
  Local<Script> script = v8_compile("(function () {"
                                    "  try {"
                                    "    throw_in_js();"
                                    "    return 42;"
                                    "  } catch (e) {"
                                    "    return e * 13;"
                                    "  }"
                                    "})();");
  CHECK_EQ(91, script->Run()->Int32Value());
}


// These are locking tests that don't need to be run again
// as part of the locking aggregation tests.
TEST(NestedLockersNoTryCatch) {
  v8::Locker locker(CcTest::isolate());
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());
  Local<v8::FunctionTemplate> fun_templ =
      v8::FunctionTemplate::New(env->GetIsolate(), ThrowInJSNoCatch);
  Local<Function> fun = fun_templ->GetFunction();
  env->Global()->Set(v8_str("throw_in_js"), fun);
  Local<Script> script = v8_compile("(function () {"
                                    "  try {"
                                    "    throw_in_js();"
                                    "    return 42;"
                                    "  } catch (e) {"
                                    "    return e * 13;"
                                    "  }"
                                    "})();");
  CHECK_EQ(91, script->Run()->Int32Value());
}


THREADED_TEST(RecursiveLocking) {
  v8::Locker locker(CcTest::isolate());
  {
    v8::Locker locker2(CcTest::isolate());
    CHECK(v8::Locker::IsLocked(CcTest::isolate()));
  }
}


static void UnlockForAMoment(const v8::FunctionCallbackInfo<v8::Value>& args) {
  ApiTestFuzzer::Fuzz();
  v8::Unlocker unlocker(CcTest::isolate());
}


THREADED_TEST(LockUnlockLock) {
  {
    v8::Locker locker(CcTest::isolate());
    v8::HandleScope scope(CcTest::isolate());
    LocalContext env;
    Local<v8::FunctionTemplate> fun_templ =
        v8::FunctionTemplate::New(CcTest::isolate(), UnlockForAMoment);
    Local<Function> fun = fun_templ->GetFunction();
    env->Global()->Set(v8_str("unlock_for_a_moment"), fun);
    Local<Script> script = v8_compile("(function () {"
                                      "  unlock_for_a_moment();"
                                      "  return 42;"
                                      "})();");
    CHECK_EQ(42, script->Run()->Int32Value());
  }
  {
    v8::Locker locker(CcTest::isolate());
    v8::HandleScope scope(CcTest::isolate());
    LocalContext env;
    Local<v8::FunctionTemplate> fun_templ =
        v8::FunctionTemplate::New(CcTest::isolate(), UnlockForAMoment);
    Local<Function> fun = fun_templ->GetFunction();
    env->Global()->Set(v8_str("unlock_for_a_moment"), fun);
    Local<Script> script = v8_compile("(function () {"
                                      "  unlock_for_a_moment();"
                                      "  return 42;"
                                      "})();");
    CHECK_EQ(42, script->Run()->Int32Value());
  }
}


static int GetGlobalObjectsCount() {
  int count = 0;
  i::HeapIterator it(CcTest::heap());
  for (i::HeapObject* object = it.next(); object != NULL; object = it.next())
    if (object->IsJSGlobalObject()) count++;
  return count;
}


static void CheckSurvivingGlobalObjectsCount(int expected) {
  // We need to collect all garbage twice to be sure that everything
  // has been collected.  This is because inline caches are cleared in
  // the first garbage collection but some of the maps have already
  // been marked at that point.  Therefore some of the maps are not
  // collected until the second garbage collection.
  CcTest::heap()->CollectAllGarbage(i::Heap::kNoGCFlags);
  CcTest::heap()->CollectAllGarbage(i::Heap::kMakeHeapIterableMask);
  int count = GetGlobalObjectsCount();
#ifdef DEBUG
  if (count != expected) CcTest::heap()->TracePathToGlobal();
#endif
  CHECK_EQ(expected, count);
}


TEST(DontLeakGlobalObjects) {
  // Regression test for issues 1139850 and 1174891.

  i::FLAG_expose_gc = true;
  v8::V8::Initialize();

  for (int i = 0; i < 5; i++) {
    { v8::HandleScope scope(CcTest::isolate());
      LocalContext context;
    }
    CcTest::isolate()->ContextDisposedNotification();
    CheckSurvivingGlobalObjectsCount(0);

    { v8::HandleScope scope(CcTest::isolate());
      LocalContext context;
      v8_compile("Date")->Run();
    }
    CcTest::isolate()->ContextDisposedNotification();
    CheckSurvivingGlobalObjectsCount(0);

    { v8::HandleScope scope(CcTest::isolate());
      LocalContext context;
      v8_compile("/aaa/")->Run();
    }
    CcTest::isolate()->ContextDisposedNotification();
    CheckSurvivingGlobalObjectsCount(0);

    { v8::HandleScope scope(CcTest::isolate());
      const char* extension_list[] = { "v8/gc" };
      v8::ExtensionConfiguration extensions(1, extension_list);
      LocalContext context(&extensions);
      v8_compile("gc();")->Run();
    }
    CcTest::isolate()->ContextDisposedNotification();
    CheckSurvivingGlobalObjectsCount(0);
  }
}


TEST(CopyablePersistent) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  i::GlobalHandles* globals =
      reinterpret_cast<i::Isolate*>(isolate)->global_handles();
  int initial_handles = globals->global_handles_count();
  typedef v8::Persistent<v8::Object, v8::CopyablePersistentTraits<v8::Object> >
      CopyableObject;
  {
    CopyableObject handle1;
    {
      v8::HandleScope scope(isolate);
      handle1.Reset(isolate, v8::Object::New(isolate));
    }
    CHECK_EQ(initial_handles + 1, globals->global_handles_count());
    CopyableObject  handle2;
    handle2 = handle1;
    CHECK(handle1 == handle2);
    CHECK_EQ(initial_handles + 2, globals->global_handles_count());
    CopyableObject handle3(handle2);
    CHECK(handle1 == handle3);
    CHECK_EQ(initial_handles + 3, globals->global_handles_count());
  }
  // Verify autodispose
  CHECK_EQ(initial_handles, globals->global_handles_count());
}


static void WeakApiCallback(
    const v8::WeakCallbackData<v8::Object, Persistent<v8::Object> >& data) {
  Local<Value> value = data.GetValue()->Get(v8_str("key"));
  CHECK_EQ(231, static_cast<int32_t>(Local<v8::Integer>::Cast(value)->Value()));
  data.GetParameter()->Reset();
  delete data.GetParameter();
}


TEST(WeakCallbackApi) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  i::GlobalHandles* globals =
      reinterpret_cast<i::Isolate*>(isolate)->global_handles();
  int initial_handles = globals->global_handles_count();
  {
    v8::HandleScope scope(isolate);
    v8::Local<v8::Object> obj = v8::Object::New(isolate);
    obj->Set(v8_str("key"), v8::Integer::New(isolate, 231));
    v8::Persistent<v8::Object>* handle =
        new v8::Persistent<v8::Object>(isolate, obj);
    handle->SetWeak<v8::Object, v8::Persistent<v8::Object> >(handle,
                                                             WeakApiCallback);
  }
  reinterpret_cast<i::Isolate*>(isolate)->heap()->
      CollectAllGarbage(i::Heap::kNoGCFlags);
  // Verify disposed.
  CHECK_EQ(initial_handles, globals->global_handles_count());
}


v8::Persistent<v8::Object> some_object;
v8::Persistent<v8::Object> bad_handle;

void NewPersistentHandleCallback(
    const v8::WeakCallbackData<v8::Object, v8::Persistent<v8::Object> >& data) {
  v8::HandleScope scope(data.GetIsolate());
  bad_handle.Reset(data.GetIsolate(), some_object);
  data.GetParameter()->Reset();
}


THREADED_TEST(NewPersistentHandleFromWeakCallback) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();

  v8::Persistent<v8::Object> handle1, handle2;
  {
    v8::HandleScope scope(isolate);
    some_object.Reset(isolate, v8::Object::New(isolate));
    handle1.Reset(isolate, v8::Object::New(isolate));
    handle2.Reset(isolate, v8::Object::New(isolate));
  }
  // Note: order is implementation dependent alas: currently
  // global handle nodes are processed by PostGarbageCollectionProcessing
  // in reverse allocation order, so if second allocated handle is deleted,
  // weak callback of the first handle would be able to 'reallocate' it.
  handle1.SetWeak(&handle1, NewPersistentHandleCallback);
  handle2.Reset();
  CcTest::heap()->CollectAllGarbage(i::Heap::kAbortIncrementalMarkingMask);
}


v8::Persistent<v8::Object> to_be_disposed;

void DisposeAndForceGcCallback(
    const v8::WeakCallbackData<v8::Object, v8::Persistent<v8::Object> >& data) {
  to_be_disposed.Reset();
  CcTest::heap()->CollectAllGarbage(i::Heap::kNoGCFlags);
  data.GetParameter()->Reset();
}


THREADED_TEST(DoNotUseDeletedNodesInSecondLevelGc) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();

  v8::Persistent<v8::Object> handle1, handle2;
  {
    v8::HandleScope scope(isolate);
    handle1.Reset(isolate, v8::Object::New(isolate));
    handle2.Reset(isolate, v8::Object::New(isolate));
  }
  handle1.SetWeak(&handle1, DisposeAndForceGcCallback);
  to_be_disposed.Reset(isolate, handle2);
  CcTest::heap()->CollectAllGarbage(i::Heap::kAbortIncrementalMarkingMask);
}

void DisposingCallback(
    const v8::WeakCallbackData<v8::Object, v8::Persistent<v8::Object> >& data) {
  data.GetParameter()->Reset();
}

void HandleCreatingCallback(
    const v8::WeakCallbackData<v8::Object, v8::Persistent<v8::Object> >& data) {
  v8::HandleScope scope(data.GetIsolate());
  v8::Persistent<v8::Object>(data.GetIsolate(),
                             v8::Object::New(data.GetIsolate()));
  data.GetParameter()->Reset();
}


THREADED_TEST(NoGlobalHandlesOrphaningDueToWeakCallback) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();

  v8::Persistent<v8::Object> handle1, handle2, handle3;
  {
    v8::HandleScope scope(isolate);
    handle3.Reset(isolate, v8::Object::New(isolate));
    handle2.Reset(isolate, v8::Object::New(isolate));
    handle1.Reset(isolate, v8::Object::New(isolate));
  }
  handle2.SetWeak(&handle2, DisposingCallback);
  handle3.SetWeak(&handle3, HandleCreatingCallback);
  CcTest::heap()->CollectAllGarbage(i::Heap::kAbortIncrementalMarkingMask);
}


THREADED_TEST(CheckForCrossContextObjectLiterals) {
  v8::V8::Initialize();

  const int nof = 2;
  const char* sources[nof] = {
    "try { [ 2, 3, 4 ].forEach(5); } catch(e) { e.toString(); }",
    "Object()"
  };

  for (int i = 0; i < nof; i++) {
    const char* source = sources[i];
    { v8::HandleScope scope(CcTest::isolate());
      LocalContext context;
      CompileRun(source);
    }
    { v8::HandleScope scope(CcTest::isolate());
      LocalContext context;
      CompileRun(source);
    }
  }
}


static v8::Handle<Value> NestedScope(v8::Local<Context> env) {
  v8::EscapableHandleScope inner(env->GetIsolate());
  env->Enter();
  v8::Local<Value> three = v8_num(3);
  v8::Local<Value> value = inner.Escape(three);
  env->Exit();
  return value;
}


THREADED_TEST(NestedHandleScopeAndContexts) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope outer(isolate);
  v8::Local<Context> env = Context::New(isolate);
  env->Enter();
  v8::Handle<Value> value = NestedScope(env);
  v8::Handle<String> str(value->ToString(isolate));
  CHECK(!str.IsEmpty());
  env->Exit();
}


static bool MatchPointers(void* key1, void* key2) {
  return key1 == key2;
}


struct SymbolInfo {
  size_t id;
  size_t size;
  std::string name;
};


class SetFunctionEntryHookTest {
 public:
  SetFunctionEntryHookTest() {
    CHECK(instance_ == NULL);
    instance_ = this;
  }
  ~SetFunctionEntryHookTest() {
    CHECK(instance_ == this);
    instance_ = NULL;
  }
  void Reset() {
    symbols_.clear();
    symbol_locations_.clear();
    invocations_.clear();
  }
  void RunTest();
  void OnJitEvent(const v8::JitCodeEvent* event);
  static void JitEvent(const v8::JitCodeEvent* event) {
    CHECK(instance_ != NULL);
    instance_->OnJitEvent(event);
  }

  void OnEntryHook(uintptr_t function,
                   uintptr_t return_addr_location);
  static void EntryHook(uintptr_t function,
                        uintptr_t return_addr_location) {
    CHECK(instance_ != NULL);
    instance_->OnEntryHook(function, return_addr_location);
  }

  static void RuntimeCallback(const v8::FunctionCallbackInfo<v8::Value>& args) {
    CHECK(instance_ != NULL);
    args.GetReturnValue().Set(v8_num(42));
  }
  void RunLoopInNewEnv(v8::Isolate* isolate);

  // Records addr as location of symbol.
  void InsertSymbolAt(i::Address addr, SymbolInfo* symbol);

  // Finds the symbol containing addr
  SymbolInfo* FindSymbolForAddr(i::Address addr);
  // Returns the number of invocations where the caller name contains
  // \p caller_name and the function name contains \p function_name.
  int CountInvocations(const char* caller_name,
                       const char* function_name);

  i::Handle<i::JSFunction> foo_func_;
  i::Handle<i::JSFunction> bar_func_;

  typedef std::map<size_t, SymbolInfo> SymbolMap;
  typedef std::map<i::Address, SymbolInfo*> SymbolLocationMap;
  typedef std::map<std::pair<SymbolInfo*, SymbolInfo*>, int> InvocationMap;
  SymbolMap symbols_;
  SymbolLocationMap symbol_locations_;
  InvocationMap invocations_;

  static SetFunctionEntryHookTest* instance_;
};
SetFunctionEntryHookTest* SetFunctionEntryHookTest::instance_ = NULL;


// Returns true if addr is in the range [start, start+len).
static bool Overlaps(i::Address start, size_t len, i::Address addr) {
  if (start <= addr && start + len > addr)
    return true;

  return false;
}

void SetFunctionEntryHookTest::InsertSymbolAt(i::Address addr,
                                              SymbolInfo* symbol) {
  // Insert the symbol at the new location.
  SymbolLocationMap::iterator it =
      symbol_locations_.insert(std::make_pair(addr, symbol)).first;
  // Now erase symbols to the left and right that overlap this one.
  while (it != symbol_locations_.begin()) {
    SymbolLocationMap::iterator left = it;
    --left;
    if (!Overlaps(left->first, left->second->size, addr))
      break;
    symbol_locations_.erase(left);
  }

  // Now erase symbols to the left and right that overlap this one.
  while (true) {
    SymbolLocationMap::iterator right = it;
    ++right;
    if (right == symbol_locations_.end())
        break;
    if (!Overlaps(addr, symbol->size, right->first))
      break;
    symbol_locations_.erase(right);
  }
}


void SetFunctionEntryHookTest::OnJitEvent(const v8::JitCodeEvent* event) {
  switch (event->type) {
    case v8::JitCodeEvent::CODE_ADDED: {
        CHECK(event->code_start != NULL);
        CHECK_NE(0, static_cast<int>(event->code_len));
        CHECK(event->name.str != NULL);
        size_t symbol_id = symbols_.size();

        // Record the new symbol.
        SymbolInfo& info = symbols_[symbol_id];
        info.id = symbol_id;
        info.size = event->code_len;
        info.name.assign(event->name.str, event->name.str + event->name.len);

        // And record it's location.
        InsertSymbolAt(reinterpret_cast<i::Address>(event->code_start), &info);
      }
      break;

    case v8::JitCodeEvent::CODE_MOVED: {
        // We would like to never see code move that we haven't seen before,
        // but the code creation event does not happen until the line endings
        // have been calculated (this is so that we can report the line in the
        // script at which the function source is found, see
        // Compiler::RecordFunctionCompilation) and the line endings
        // calculations can cause a GC, which can move the newly created code
        // before its existence can be logged.
        SymbolLocationMap::iterator it(
            symbol_locations_.find(
                reinterpret_cast<i::Address>(event->code_start)));
        if (it != symbol_locations_.end()) {
          // Found a symbol at this location, move it.
          SymbolInfo* info = it->second;
          symbol_locations_.erase(it);
          InsertSymbolAt(reinterpret_cast<i::Address>(event->new_code_start),
                         info);
        }
      }
    default:
      break;
  }
}

void SetFunctionEntryHookTest::OnEntryHook(
    uintptr_t function, uintptr_t return_addr_location) {
  // Get the function's code object.
  i::Code* function_code = i::Code::GetCodeFromTargetAddress(
      reinterpret_cast<i::Address>(function));
  CHECK(function_code != NULL);

  // Then try and look up the caller's code object.
  i::Address caller = *reinterpret_cast<i::Address*>(return_addr_location);

  // Count the invocation.
  SymbolInfo* caller_symbol = FindSymbolForAddr(caller);
  SymbolInfo* function_symbol =
      FindSymbolForAddr(reinterpret_cast<i::Address>(function));
  ++invocations_[std::make_pair(caller_symbol, function_symbol)];

  if (!bar_func_.is_null() && function_code == bar_func_->code()) {
    // Check that we have a symbol for the "bar" function at the right location.
    SymbolLocationMap::iterator it(
        symbol_locations_.find(function_code->instruction_start()));
    CHECK(it != symbol_locations_.end());
  }

  if (!foo_func_.is_null() && function_code == foo_func_->code()) {
    // Check that we have a symbol for "foo" at the right location.
    SymbolLocationMap::iterator it(
        symbol_locations_.find(function_code->instruction_start()));
    CHECK(it != symbol_locations_.end());
  }
}


SymbolInfo* SetFunctionEntryHookTest::FindSymbolForAddr(i::Address addr) {
  SymbolLocationMap::iterator it(symbol_locations_.lower_bound(addr));
  // Do we have a direct hit on a symbol?
  if (it != symbol_locations_.end()) {
    if (it->first == addr)
      return it->second;
  }

  // If not a direct hit, it'll have to be the previous symbol.
  if (it == symbol_locations_.begin())
    return NULL;

  --it;
  size_t offs = addr - it->first;
  if (offs < it->second->size)
    return it->second;

  return NULL;
}


int SetFunctionEntryHookTest::CountInvocations(
    const char* caller_name, const char* function_name) {
  InvocationMap::iterator it(invocations_.begin());
  int invocations = 0;
  for (; it != invocations_.end(); ++it) {
    SymbolInfo* caller = it->first.first;
    SymbolInfo* function = it->first.second;

    // Filter out non-matching functions.
    if (function_name != NULL) {
      if (function->name.find(function_name) == std::string::npos)
        continue;
    }

    // Filter out non-matching callers.
    if (caller_name != NULL) {
      if (caller == NULL)
        continue;
      if (caller->name.find(caller_name) == std::string::npos)
        continue;
    }

    // It matches add the invocation count to the tally.
    invocations += it->second;
  }

  return invocations;
}


void SetFunctionEntryHookTest::RunLoopInNewEnv(v8::Isolate* isolate) {
  v8::HandleScope outer(isolate);
  v8::Local<Context> env = Context::New(isolate);
  env->Enter();

  Local<ObjectTemplate> t = ObjectTemplate::New(isolate);
  t->Set(v8_str("asdf"), v8::FunctionTemplate::New(isolate, RuntimeCallback));
  env->Global()->Set(v8_str("obj"), t->NewInstance());

  const char* script =
      "function bar() {\n"
      "  var sum = 0;\n"
      "  for (i = 0; i < 100; ++i)\n"
      "    sum = foo(i);\n"
      "  return sum;\n"
      "}\n"
      "function foo(i) { return i * i; }\n"
      "// Invoke on the runtime function.\n"
      "obj.asdf()";
  CompileRun(script);
  bar_func_ = i::Handle<i::JSFunction>::cast(
          v8::Utils::OpenHandle(*env->Global()->Get(v8_str("bar"))));
  DCHECK(!bar_func_.is_null());

  foo_func_ =
      i::Handle<i::JSFunction>::cast(
           v8::Utils::OpenHandle(*env->Global()->Get(v8_str("foo"))));
  DCHECK(!foo_func_.is_null());

  v8::Handle<v8::Value> value = CompileRun("bar();");
  CHECK(value->IsNumber());
  CHECK_EQ(9801.0, v8::Number::Cast(*value)->Value());

  // Test the optimized codegen path.
  value = CompileRun("%OptimizeFunctionOnNextCall(foo);"
                     "bar();");
  CHECK(value->IsNumber());
  CHECK_EQ(9801.0, v8::Number::Cast(*value)->Value());

  env->Exit();
}


void SetFunctionEntryHookTest::RunTest() {
  // Work in a new isolate throughout.
  v8::Isolate::CreateParams create_params;
  create_params.entry_hook = EntryHook;
  create_params.code_event_handler = JitEvent;
  v8::Isolate* isolate = v8::Isolate::New(create_params);

  {
    v8::Isolate::Scope scope(isolate);

    RunLoopInNewEnv(isolate);

    // Check the exepected invocation counts.
    CHECK_EQ(2, CountInvocations(NULL, "bar"));
    CHECK_EQ(200, CountInvocations("bar", "foo"));
    CHECK_EQ(200, CountInvocations(NULL, "foo"));

    // Verify that we have an entry hook on some specific stubs.
    CHECK_NE(0, CountInvocations(NULL, "CEntryStub"));
    CHECK_NE(0, CountInvocations(NULL, "JSEntryStub"));
    CHECK_NE(0, CountInvocations(NULL, "JSEntryTrampoline"));
  }
  isolate->Dispose();

  Reset();

  // Make sure a second isolate is unaffected by the previous entry hook.
  isolate = v8::Isolate::New();
  {
    v8::Isolate::Scope scope(isolate);

    // Reset the entry count to zero and set the entry hook.
    RunLoopInNewEnv(isolate);

    // We should record no invocations in this isolate.
    CHECK_EQ(0, static_cast<int>(invocations_.size()));
  }

  isolate->Dispose();
}


TEST(SetFunctionEntryHook) {
  // FunctionEntryHook does not work well with experimental natives.
  // Experimental natives are compiled during snapshot deserialization.
  // This test breaks because InstallGetter (function from snapshot that
  // only gets called from experimental natives) is compiled with entry hooks.
  i::FLAG_allow_natives_syntax = true;
  i::FLAG_use_inlining = false;

  SetFunctionEntryHookTest test;
  test.RunTest();
}


static i::HashMap* code_map = NULL;
static i::HashMap* jitcode_line_info = NULL;
static int saw_bar = 0;
static int move_events = 0;


static bool FunctionNameIs(const char* expected,
                           const v8::JitCodeEvent* event) {
  // Log lines for functions are of the general form:
  // "LazyCompile:<type><function_name>", where the type is one of
  // "*", "~" or "".
  static const char kPreamble[] = "LazyCompile:";
  static size_t kPreambleLen = sizeof(kPreamble) - 1;

  if (event->name.len < sizeof(kPreamble) - 1 ||
      strncmp(kPreamble, event->name.str, kPreambleLen) != 0) {
    return false;
  }

  const char* tail = event->name.str + kPreambleLen;
  size_t tail_len = event->name.len - kPreambleLen;
  size_t expected_len = strlen(expected);
  if (tail_len > 1 && (*tail == '*' || *tail == '~')) {
    --tail_len;
    ++tail;
  }

  // Check for tails like 'bar :1'.
  if (tail_len > expected_len + 2 &&
      tail[expected_len] == ' ' &&
      tail[expected_len + 1] == ':' &&
      tail[expected_len + 2] &&
      !strncmp(tail, expected, expected_len)) {
    return true;
  }

  if (tail_len != expected_len)
    return false;

  return strncmp(tail, expected, expected_len) == 0;
}


static void event_handler(const v8::JitCodeEvent* event) {
  CHECK(event != NULL);
  CHECK(code_map != NULL);
  CHECK(jitcode_line_info != NULL);

  class DummyJitCodeLineInfo {
  };

  switch (event->type) {
    case v8::JitCodeEvent::CODE_ADDED: {
        CHECK(event->code_start != NULL);
        CHECK_NE(0, static_cast<int>(event->code_len));
        CHECK(event->name.str != NULL);
        i::HashMap::Entry* entry =
            code_map->Lookup(event->code_start,
                             i::ComputePointerHash(event->code_start),
                             true);
        entry->value = reinterpret_cast<void*>(event->code_len);

        if (FunctionNameIs("bar", event)) {
          ++saw_bar;
        }
      }
      break;

    case v8::JitCodeEvent::CODE_MOVED: {
        uint32_t hash = i::ComputePointerHash(event->code_start);
        // We would like to never see code move that we haven't seen before,
        // but the code creation event does not happen until the line endings
        // have been calculated (this is so that we can report the line in the
        // script at which the function source is found, see
        // Compiler::RecordFunctionCompilation) and the line endings
        // calculations can cause a GC, which can move the newly created code
        // before its existence can be logged.
        i::HashMap::Entry* entry =
            code_map->Lookup(event->code_start, hash, false);
        if (entry != NULL) {
          ++move_events;

          CHECK_EQ(reinterpret_cast<void*>(event->code_len), entry->value);
          code_map->Remove(event->code_start, hash);

          entry = code_map->Lookup(event->new_code_start,
                                   i::ComputePointerHash(event->new_code_start),
                                   true);
          CHECK(entry != NULL);
          entry->value = reinterpret_cast<void*>(event->code_len);
        }
      }
      break;

    case v8::JitCodeEvent::CODE_REMOVED:
      // Object/code removal events are currently not dispatched from the GC.
      CHECK(false);
      break;

    // For CODE_START_LINE_INFO_RECORDING event, we will create one
    // DummyJitCodeLineInfo data structure pointed by event->user_dat. We
    // record it in jitcode_line_info.
    case v8::JitCodeEvent::CODE_START_LINE_INFO_RECORDING: {
        DummyJitCodeLineInfo* line_info = new DummyJitCodeLineInfo();
        v8::JitCodeEvent* temp_event = const_cast<v8::JitCodeEvent*>(event);
        temp_event->user_data = line_info;
        i::HashMap::Entry* entry =
            jitcode_line_info->Lookup(line_info,
                                      i::ComputePointerHash(line_info),
                                      true);
        entry->value = reinterpret_cast<void*>(line_info);
      }
      break;
    // For these two events, we will check whether the event->user_data
    // data structure is created before during CODE_START_LINE_INFO_RECORDING
    // event. And delete it in CODE_END_LINE_INFO_RECORDING event handling.
    case v8::JitCodeEvent::CODE_END_LINE_INFO_RECORDING: {
        CHECK(event->user_data != NULL);
        uint32_t hash = i::ComputePointerHash(event->user_data);
        i::HashMap::Entry* entry =
            jitcode_line_info->Lookup(event->user_data, hash, false);
        CHECK(entry != NULL);
        delete reinterpret_cast<DummyJitCodeLineInfo*>(event->user_data);
      }
      break;

    case v8::JitCodeEvent::CODE_ADD_LINE_POS_INFO: {
        CHECK(event->user_data != NULL);
        uint32_t hash = i::ComputePointerHash(event->user_data);
        i::HashMap::Entry* entry =
            jitcode_line_info->Lookup(event->user_data, hash, false);
        CHECK(entry != NULL);
      }
      break;

    default:
      // Impossible event.
      CHECK(false);
      break;
  }
}


UNINITIALIZED_TEST(SetJitCodeEventHandler) {
  i::FLAG_stress_compaction = true;
  i::FLAG_incremental_marking = false;
  if (i::FLAG_never_compact) return;
  const char* script =
      "function bar() {"
      "  var sum = 0;"
      "  for (i = 0; i < 10; ++i)"
      "    sum = foo(i);"
      "  return sum;"
      "}"
      "function foo(i) { return i; };"
      "bar();";

  // Run this test in a new isolate to make sure we don't
  // have remnants of state from other code.
  v8::Isolate* isolate = v8::Isolate::New();
  isolate->Enter();
  i::Isolate* i_isolate = reinterpret_cast<i::Isolate*>(isolate);
  i::Heap* heap = i_isolate->heap();

  // Start with a clean slate.
  heap->CollectAllAvailableGarbage("TestSetJitCodeEventHandler_Prepare");

  {
    v8::HandleScope scope(isolate);
    i::HashMap code(MatchPointers);
    code_map = &code;

    i::HashMap lineinfo(MatchPointers);
    jitcode_line_info = &lineinfo;

    saw_bar = 0;
    move_events = 0;

    isolate->SetJitCodeEventHandler(v8::kJitCodeEventDefault, event_handler);

    // Generate new code objects sparsely distributed across several
    // different fragmented code-space pages.
    const int kIterations = 10;
    for (int i = 0; i < kIterations; ++i) {
      LocalContext env(isolate);
      i::AlwaysAllocateScope always_allocate(i_isolate);
      SimulateFullSpace(heap->code_space());
      CompileRun(script);

      // Keep a strong reference to the code object in the handle scope.
      i::Handle<i::Code> bar_code(i::Handle<i::JSFunction>::cast(
          v8::Utils::OpenHandle(*env->Global()->Get(v8_str("bar"))))->code());
      i::Handle<i::Code> foo_code(i::Handle<i::JSFunction>::cast(
          v8::Utils::OpenHandle(*env->Global()->Get(v8_str("foo"))))->code());

      // Clear the compilation cache to get more wastage.
      reinterpret_cast<i::Isolate*>(isolate)->compilation_cache()->Clear();
    }

    // Force code movement.
    heap->CollectAllAvailableGarbage("TestSetJitCodeEventHandler_Move");

    isolate->SetJitCodeEventHandler(v8::kJitCodeEventDefault, NULL);

    CHECK_LE(kIterations, saw_bar);
    CHECK_LT(0, move_events);

    code_map = NULL;
    jitcode_line_info = NULL;
  }

  isolate->Exit();
  isolate->Dispose();

  // Do this in a new isolate.
  isolate = v8::Isolate::New();
  isolate->Enter();

  // Verify that we get callbacks for existing code objects when we
  // request enumeration of existing code.
  {
    v8::HandleScope scope(isolate);
    LocalContext env(isolate);
    CompileRun(script);

    // Now get code through initial iteration.
    i::HashMap code(MatchPointers);
    code_map = &code;

    i::HashMap lineinfo(MatchPointers);
    jitcode_line_info = &lineinfo;

    isolate->SetJitCodeEventHandler(v8::kJitCodeEventEnumExisting,
                                    event_handler);
    isolate->SetJitCodeEventHandler(v8::kJitCodeEventDefault, NULL);

    jitcode_line_info = NULL;
    // We expect that we got some events. Note that if we could get code removal
    // notifications, we could compare two collections, one created by listening
    // from the time of creation of an isolate, and the other by subscribing
    // with EnumExisting.
    CHECK_LT(0u, code.occupancy());

    code_map = NULL;
  }

  isolate->Exit();
  isolate->Dispose();
}


THREADED_TEST(ExternalAllocatedMemory) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope outer(isolate);
  v8::Local<Context> env(Context::New(isolate));
  CHECK(!env.IsEmpty());
  const int64_t kSize = 1024*1024;
  int64_t baseline = isolate->AdjustAmountOfExternalAllocatedMemory(0);
  CHECK_EQ(baseline + kSize,
           isolate->AdjustAmountOfExternalAllocatedMemory(kSize));
  CHECK_EQ(baseline,
           isolate->AdjustAmountOfExternalAllocatedMemory(-kSize));
  const int64_t kTriggerGCSize =
      v8::internal::Internals::kExternalAllocationLimit + 1;
  CHECK_EQ(baseline + kTriggerGCSize,
           isolate->AdjustAmountOfExternalAllocatedMemory(kTriggerGCSize));
  CHECK_EQ(baseline,
           isolate->AdjustAmountOfExternalAllocatedMemory(-kTriggerGCSize));
}


// Regression test for issue 54, object templates with internal fields
// but no accessors or interceptors did not get their internal field
// count set on instances.
THREADED_TEST(Regress54) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope outer(isolate);
  static v8::Persistent<v8::ObjectTemplate> templ;
  if (templ.IsEmpty()) {
    v8::EscapableHandleScope inner(isolate);
    v8::Local<v8::ObjectTemplate> local = v8::ObjectTemplate::New(isolate);
    local->SetInternalFieldCount(1);
    templ.Reset(isolate, inner.Escape(local));
  }
  v8::Handle<v8::Object> result =
      v8::Local<v8::ObjectTemplate>::New(isolate, templ)->NewInstance();
  CHECK_EQ(1, result->InternalFieldCount());
}


// If part of the threaded tests, this test makes ThreadingTest fail
// on mac.
TEST(CatchStackOverflow) {
  LocalContext context;
  v8::HandleScope scope(context->GetIsolate());
  v8::TryCatch try_catch;
  v8::Handle<v8::Value> result = CompileRun(
    "function f() {"
    "  return f();"
    "}"
    ""
    "f();");
  CHECK(result.IsEmpty());
}


static void CheckTryCatchSourceInfo(v8::Handle<v8::Script> script,
                                    const char* resource_name,
                                    int line_offset) {
  v8::HandleScope scope(CcTest::isolate());
  v8::TryCatch try_catch;
  v8::Handle<v8::Value> result = script->Run();
  CHECK(result.IsEmpty());
  CHECK(try_catch.HasCaught());
  v8::Handle<v8::Message> message = try_catch.Message();
  CHECK(!message.IsEmpty());
  CHECK_EQ(10 + line_offset, message->GetLineNumber());
  CHECK_EQ(91, message->GetStartPosition());
  CHECK_EQ(92, message->GetEndPosition());
  CHECK_EQ(2, message->GetStartColumn());
  CHECK_EQ(3, message->GetEndColumn());
  v8::String::Utf8Value line(message->GetSourceLine());
  CHECK_EQ(0, strcmp("  throw 'nirk';", *line));
  v8::String::Utf8Value name(message->GetScriptOrigin().ResourceName());
  CHECK_EQ(0, strcmp(resource_name, *name));
}


THREADED_TEST(TryCatchSourceInfo) {
  LocalContext context;
  v8::HandleScope scope(context->GetIsolate());
  v8::Local<v8::String> source = v8_str(
      "function Foo() {\n"
      "  return Bar();\n"
      "}\n"
      "\n"
      "function Bar() {\n"
      "  return Baz();\n"
      "}\n"
      "\n"
      "function Baz() {\n"
      "  throw 'nirk';\n"
      "}\n"
      "\n"
      "Foo();\n");

  const char* resource_name;
  v8::Handle<v8::Script> script;
  resource_name = "test.js";
  script = CompileWithOrigin(source, resource_name);
  CheckTryCatchSourceInfo(script, resource_name, 0);

  resource_name = "test1.js";
  v8::ScriptOrigin origin1(
      v8::String::NewFromUtf8(context->GetIsolate(), resource_name));
  script = v8::Script::Compile(source, &origin1);
  CheckTryCatchSourceInfo(script, resource_name, 0);

  resource_name = "test2.js";
  v8::ScriptOrigin origin2(
      v8::String::NewFromUtf8(context->GetIsolate(), resource_name),
      v8::Integer::New(context->GetIsolate(), 7));
  script = v8::Script::Compile(source, &origin2);
  CheckTryCatchSourceInfo(script, resource_name, 7);
}


THREADED_TEST(CompilationCache) {
  LocalContext context;
  v8::HandleScope scope(context->GetIsolate());
  v8::Handle<v8::String> source0 =
      v8::String::NewFromUtf8(context->GetIsolate(), "1234");
  v8::Handle<v8::String> source1 =
      v8::String::NewFromUtf8(context->GetIsolate(), "1234");
  v8::Handle<v8::Script> script0 = CompileWithOrigin(source0, "test.js");
  v8::Handle<v8::Script> script1 = CompileWithOrigin(source1, "test.js");
  v8::Handle<v8::Script> script2 =
      v8::Script::Compile(source0);  // different origin
  CHECK_EQ(1234, script0->Run()->Int32Value());
  CHECK_EQ(1234, script1->Run()->Int32Value());
  CHECK_EQ(1234, script2->Run()->Int32Value());
}


static void FunctionNameCallback(
    const v8::FunctionCallbackInfo<v8::Value>& args) {
  ApiTestFuzzer::Fuzz();
  args.GetReturnValue().Set(v8_num(42));
}


THREADED_TEST(CallbackFunctionName) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);
  Local<ObjectTemplate> t = ObjectTemplate::New(isolate);
  t->Set(v8_str("asdf"),
         v8::FunctionTemplate::New(isolate, FunctionNameCallback));
  context->Global()->Set(v8_str("obj"), t->NewInstance());
  v8::Handle<v8::Value> value = CompileRun("obj.asdf.name");
  CHECK(value->IsString());
  v8::String::Utf8Value name(value);
  CHECK_EQ(0, strcmp("asdf", *name));
}


THREADED_TEST(DateAccess) {
  LocalContext context;
  v8::HandleScope scope(context->GetIsolate());
  v8::Handle<v8::Value> date =
      v8::Date::New(context->GetIsolate(), 1224744689038.0);
  CHECK(date->IsDate());
  CHECK_EQ(1224744689038.0, date.As<v8::Date>()->ValueOf());
}


void CheckProperties(v8::Isolate* isolate, v8::Handle<v8::Value> val,
                     unsigned elmc, const char* elmv[]) {
  v8::Handle<v8::Object> obj = val.As<v8::Object>();
  v8::Handle<v8::Array> props = obj->GetPropertyNames();
  CHECK_EQ(elmc, props->Length());
  for (unsigned i = 0; i < elmc; i++) {
    v8::String::Utf8Value elm(props->Get(v8::Integer::New(isolate, i)));
    CHECK_EQ(0, strcmp(elmv[i], *elm));
  }
}


void CheckOwnProperties(v8::Isolate* isolate, v8::Handle<v8::Value> val,
                        unsigned elmc, const char* elmv[]) {
  v8::Handle<v8::Object> obj = val.As<v8::Object>();
  v8::Handle<v8::Array> props = obj->GetOwnPropertyNames();
  CHECK_EQ(elmc, props->Length());
  for (unsigned i = 0; i < elmc; i++) {
    v8::String::Utf8Value elm(props->Get(v8::Integer::New(isolate, i)));
    CHECK_EQ(0, strcmp(elmv[i], *elm));
  }
}


THREADED_TEST(PropertyEnumeration) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);
  v8::Handle<v8::Value> obj = CompileRun(
      "var result = [];"
      "result[0] = {};"
      "result[1] = {a: 1, b: 2};"
      "result[2] = [1, 2, 3];"
      "var proto = {x: 1, y: 2, z: 3};"
      "var x = { __proto__: proto, w: 0, z: 1 };"
      "result[3] = x;"
      "result;");
  v8::Handle<v8::Array> elms = obj.As<v8::Array>();
  CHECK_EQ(4u, elms->Length());
  int elmc0 = 0;
  const char** elmv0 = NULL;
  CheckProperties(
      isolate, elms->Get(v8::Integer::New(isolate, 0)), elmc0, elmv0);
  CheckOwnProperties(
      isolate, elms->Get(v8::Integer::New(isolate, 0)), elmc0, elmv0);
  int elmc1 = 2;
  const char* elmv1[] = {"a", "b"};
  CheckProperties(
      isolate, elms->Get(v8::Integer::New(isolate, 1)), elmc1, elmv1);
  CheckOwnProperties(
      isolate, elms->Get(v8::Integer::New(isolate, 1)), elmc1, elmv1);
  int elmc2 = 3;
  const char* elmv2[] = {"0", "1", "2"};
  CheckProperties(
      isolate, elms->Get(v8::Integer::New(isolate, 2)), elmc2, elmv2);
  CheckOwnProperties(
      isolate, elms->Get(v8::Integer::New(isolate, 2)), elmc2, elmv2);
  int elmc3 = 4;
  const char* elmv3[] = {"w", "z", "x", "y"};
  CheckProperties(
      isolate, elms->Get(v8::Integer::New(isolate, 3)), elmc3, elmv3);
  int elmc4 = 2;
  const char* elmv4[] = {"w", "z"};
  CheckOwnProperties(
      isolate, elms->Get(v8::Integer::New(isolate, 3)), elmc4, elmv4);
}


THREADED_TEST(PropertyEnumeration2) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);
  v8::Handle<v8::Value> obj = CompileRun(
      "var result = [];"
      "result[0] = {};"
      "result[1] = {a: 1, b: 2};"
      "result[2] = [1, 2, 3];"
      "var proto = {x: 1, y: 2, z: 3};"
      "var x = { __proto__: proto, w: 0, z: 1 };"
      "result[3] = x;"
      "result;");
  v8::Handle<v8::Array> elms = obj.As<v8::Array>();
  CHECK_EQ(4u, elms->Length());
  int elmc0 = 0;
  const char** elmv0 = NULL;
  CheckProperties(isolate,
                  elms->Get(v8::Integer::New(isolate, 0)), elmc0, elmv0);

  v8::Handle<v8::Value> val = elms->Get(v8::Integer::New(isolate, 0));
  v8::Handle<v8::Array> props = val.As<v8::Object>()->GetPropertyNames();
  CHECK_EQ(0u, props->Length());
  for (uint32_t i = 0; i < props->Length(); i++) {
    printf("p[%u]\n", i);
  }
}


THREADED_TEST(AccessChecksReenabledCorrectly) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);
  Local<ObjectTemplate> templ = ObjectTemplate::New(isolate);
  templ->SetAccessCheckCallbacks(AccessAlwaysBlocked, NULL);
  templ->Set(v8_str("a"), v8_str("a"));
  // Add more than 8 (see kMaxFastProperties) properties
  // so that the constructor will force copying map.
  // Cannot sprintf, gcc complains unsafety.
  char buf[4];
  for (char i = '0'; i <= '9' ; i++) {
    buf[0] = i;
    for (char j = '0'; j <= '9'; j++) {
      buf[1] = j;
      for (char k = '0'; k <= '9'; k++) {
        buf[2] = k;
        buf[3] = 0;
        templ->Set(v8_str(buf), v8::Number::New(isolate, k));
      }
    }
  }

  Local<v8::Object> instance_1 = templ->NewInstance();
  context->Global()->Set(v8_str("obj_1"), instance_1);

  Local<Value> value_1 = CompileRun("obj_1.a");
  CHECK(value_1.IsEmpty());

  Local<v8::Object> instance_2 = templ->NewInstance();
  context->Global()->Set(v8_str("obj_2"), instance_2);

  Local<Value> value_2 = CompileRun("obj_2.a");
  CHECK(value_2.IsEmpty());
}


THREADED_TEST(TurnOnAccessCheck) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope handle_scope(isolate);

  // Create an environment with access check to the global object disabled by
  // default.
  v8::Handle<v8::ObjectTemplate> global_template =
      v8::ObjectTemplate::New(isolate);
  global_template->SetAccessCheckCallbacks(AccessAlwaysBlocked, NULL,
                                           v8::Handle<v8::Value>(), false);
  v8::Local<Context> context = Context::New(isolate, NULL, global_template);
  Context::Scope context_scope(context);

  // Set up a property and a number of functions.
  context->Global()->Set(v8_str("a"), v8_num(1));
  CompileRun("function f1() {return a;}"
             "function f2() {return a;}"
             "function g1() {return h();}"
             "function g2() {return h();}"
             "function h() {return 1;}");
  Local<Function> f1 =
      Local<Function>::Cast(context->Global()->Get(v8_str("f1")));
  Local<Function> f2 =
      Local<Function>::Cast(context->Global()->Get(v8_str("f2")));
  Local<Function> g1 =
      Local<Function>::Cast(context->Global()->Get(v8_str("g1")));
  Local<Function> g2 =
      Local<Function>::Cast(context->Global()->Get(v8_str("g2")));
  Local<Function> h =
      Local<Function>::Cast(context->Global()->Get(v8_str("h")));

  // Get the global object.
  v8::Handle<v8::Object> global = context->Global();

  // Call f1 one time and f2 a number of times. This will ensure that f1 still
  // uses the runtime system to retreive property a whereas f2 uses global load
  // inline cache.
  CHECK(f1->Call(global, 0, NULL)->Equals(v8_num(1)));
  for (int i = 0; i < 4; i++) {
    CHECK(f2->Call(global, 0, NULL)->Equals(v8_num(1)));
  }

  // Same for g1 and g2.
  CHECK(g1->Call(global, 0, NULL)->Equals(v8_num(1)));
  for (int i = 0; i < 4; i++) {
    CHECK(g2->Call(global, 0, NULL)->Equals(v8_num(1)));
  }

  // Detach the global and turn on access check.
  Local<Object> hidden_global = Local<Object>::Cast(
      context->Global()->GetPrototype());
  context->DetachGlobal();
  hidden_global->TurnOnAccessCheck();

  // Failing access check results in exception.
  CHECK(f1->Call(global, 0, NULL).IsEmpty());
  CHECK(f2->Call(global, 0, NULL).IsEmpty());
  CHECK(g1->Call(global, 0, NULL).IsEmpty());
  CHECK(g2->Call(global, 0, NULL).IsEmpty());

  // No failing access check when just returning a constant.
  CHECK(h->Call(global, 0, NULL)->Equals(v8_num(1)));
}


// Tests that ScriptData can be serialized and deserialized.
TEST(PreCompileSerialization) {
  v8::V8::Initialize();
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  HandleScope handle_scope(isolate);

  i::FLAG_min_preparse_length = 0;
  const char* script = "function foo(a) { return a+1; }";
  v8::ScriptCompiler::Source source(v8_str(script));
  v8::ScriptCompiler::Compile(isolate, &source,
                              v8::ScriptCompiler::kProduceParserCache);
  // Serialize.
  const v8::ScriptCompiler::CachedData* cd = source.GetCachedData();
  i::byte* serialized_data = i::NewArray<i::byte>(cd->length);
  i::MemCopy(serialized_data, cd->data, cd->length);

  // Deserialize.
  i::ScriptData* deserialized = new i::ScriptData(serialized_data, cd->length);

  // Verify that the original is the same as the deserialized.
  CHECK_EQ(cd->length, deserialized->length());
  CHECK_EQ(0, memcmp(cd->data, deserialized->data(), cd->length));

  delete deserialized;
  i::DeleteArray(serialized_data);
}


// This tests that we do not allow dictionary load/call inline caches
// to use functions that have not yet been compiled.  The potential
// problem of loading a function that has not yet been compiled can
// arise because we share code between contexts via the compilation
// cache.
THREADED_TEST(DictionaryICLoadedFunction) {
  v8::HandleScope scope(CcTest::isolate());
  // Test LoadIC.
  for (int i = 0; i < 2; i++) {
    LocalContext context;
    context->Global()->Set(v8_str("tmp"), v8::True(CcTest::isolate()));
    context->Global()->Delete(v8_str("tmp"));
    CompileRun("for (var j = 0; j < 10; j++) new RegExp('');");
  }
  // Test CallIC.
  for (int i = 0; i < 2; i++) {
    LocalContext context;
    context->Global()->Set(v8_str("tmp"), v8::True(CcTest::isolate()));
    context->Global()->Delete(v8_str("tmp"));
    CompileRun("for (var j = 0; j < 10; j++) RegExp('')");
  }
}


// Test that cross-context new calls use the context of the callee to
// create the new JavaScript object.
THREADED_TEST(CrossContextNew) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  v8::Local<Context> context0 = Context::New(isolate);
  v8::Local<Context> context1 = Context::New(isolate);

  // Allow cross-domain access.
  Local<String> token = v8_str("<security token>");
  context0->SetSecurityToken(token);
  context1->SetSecurityToken(token);

  // Set an 'x' property on the Object prototype and define a
  // constructor function in context0.
  context0->Enter();
  CompileRun("Object.prototype.x = 42; function C() {};");
  context0->Exit();

  // Call the constructor function from context0 and check that the
  // result has the 'x' property.
  context1->Enter();
  context1->Global()->Set(v8_str("other"), context0->Global());
  Local<Value> value = CompileRun("var instance = new other.C(); instance.x");
  CHECK(value->IsInt32());
  CHECK_EQ(42, value->Int32Value());
  context1->Exit();
}


// Verify that we can clone an object
TEST(ObjectClone) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);

  const char* sample =
    "var rv = {};"      \
    "rv.alpha = 'hello';" \
    "rv.beta = 123;"     \
    "rv;";

  // Create an object, verify basics.
  Local<Value> val = CompileRun(sample);
  CHECK(val->IsObject());
  Local<v8::Object> obj = val.As<v8::Object>();
  obj->Set(v8_str("gamma"), v8_str("cloneme"));

  CHECK(v8_str("hello")->Equals(obj->Get(v8_str("alpha"))));
  CHECK(v8::Integer::New(isolate, 123)->Equals(obj->Get(v8_str("beta"))));
  CHECK(v8_str("cloneme")->Equals(obj->Get(v8_str("gamma"))));

  // Clone it.
  Local<v8::Object> clone = obj->Clone();
  CHECK(v8_str("hello")->Equals(clone->Get(v8_str("alpha"))));
  CHECK(v8::Integer::New(isolate, 123)->Equals(clone->Get(v8_str("beta"))));
  CHECK(v8_str("cloneme")->Equals(clone->Get(v8_str("gamma"))));

  // Set a property on the clone, verify each object.
  clone->Set(v8_str("beta"), v8::Integer::New(isolate, 456));
  CHECK(v8::Integer::New(isolate, 123)->Equals(obj->Get(v8_str("beta"))));
  CHECK(v8::Integer::New(isolate, 456)->Equals(clone->Get(v8_str("beta"))));
}


class OneByteVectorResource : public v8::String::ExternalOneByteStringResource {
 public:
  explicit OneByteVectorResource(i::Vector<const char> vector)
      : data_(vector) {}
  virtual ~OneByteVectorResource() {}
  virtual size_t length() const { return data_.length(); }
  virtual const char* data() const { return data_.start(); }
 private:
  i::Vector<const char> data_;
};


class UC16VectorResource : public v8::String::ExternalStringResource {
 public:
  explicit UC16VectorResource(i::Vector<const i::uc16> vector)
      : data_(vector) {}
  virtual ~UC16VectorResource() {}
  virtual size_t length() const { return data_.length(); }
  virtual const i::uc16* data() const { return data_.start(); }
 private:
  i::Vector<const i::uc16> data_;
};


static void MorphAString(i::String* string,
                         OneByteVectorResource* one_byte_resource,
                         UC16VectorResource* uc16_resource) {
  CHECK(i::StringShape(string).IsExternal());
  if (string->IsOneByteRepresentation()) {
    // Check old map is not internalized or long.
    CHECK(string->map() == CcTest::heap()->external_one_byte_string_map());
    // Morph external string to be TwoByte string.
    string->set_map(CcTest::heap()->external_string_map());
    i::ExternalTwoByteString* morphed =
         i::ExternalTwoByteString::cast(string);
    morphed->set_resource(uc16_resource);
  } else {
    // Check old map is not internalized or long.
    CHECK(string->map() == CcTest::heap()->external_string_map());
    // Morph external string to be one-byte string.
    string->set_map(CcTest::heap()->external_one_byte_string_map());
    i::ExternalOneByteString* morphed = i::ExternalOneByteString::cast(string);
    morphed->set_resource(one_byte_resource);
  }
}


// Test that we can still flatten a string if the components it is built up
// from have been turned into 16 bit strings in the mean time.
THREADED_TEST(MorphCompositeStringTest) {
  char utf_buffer[129];
  const char* c_string = "Now is the time for all good men"
                         " to come to the aid of the party";
  uint16_t* two_byte_string = AsciiToTwoByteString(c_string);
  {
    LocalContext env;
    i::Factory* factory = CcTest::i_isolate()->factory();
    v8::HandleScope scope(env->GetIsolate());
    OneByteVectorResource one_byte_resource(
        i::Vector<const char>(c_string, i::StrLength(c_string)));
    UC16VectorResource uc16_resource(
        i::Vector<const uint16_t>(two_byte_string,
                                  i::StrLength(c_string)));

    Local<String> lhs(
        v8::Utils::ToLocal(factory->NewExternalStringFromOneByte(
                                        &one_byte_resource).ToHandleChecked()));
    Local<String> rhs(
        v8::Utils::ToLocal(factory->NewExternalStringFromOneByte(
                                        &one_byte_resource).ToHandleChecked()));

    env->Global()->Set(v8_str("lhs"), lhs);
    env->Global()->Set(v8_str("rhs"), rhs);

    CompileRun(
        "var cons = lhs + rhs;"
        "var slice = lhs.substring(1, lhs.length - 1);"
        "var slice_on_cons = (lhs + rhs).substring(1, lhs.length *2 - 1);");

    CHECK(lhs->IsOneByte());
    CHECK(rhs->IsOneByte());

    MorphAString(*v8::Utils::OpenHandle(*lhs), &one_byte_resource,
                 &uc16_resource);
    MorphAString(*v8::Utils::OpenHandle(*rhs), &one_byte_resource,
                 &uc16_resource);

    // This should UTF-8 without flattening, since everything is ASCII.
    Handle<String> cons = v8_compile("cons")->Run().As<String>();
    CHECK_EQ(128, cons->Utf8Length());
    int nchars = -1;
    CHECK_EQ(129, cons->WriteUtf8(utf_buffer, -1, &nchars));
    CHECK_EQ(128, nchars);
    CHECK_EQ(0, strcmp(
        utf_buffer,
        "Now is the time for all good men to come to the aid of the party"
        "Now is the time for all good men to come to the aid of the party"));

    // Now do some stuff to make sure the strings are flattened, etc.
    CompileRun(
        "/[^a-z]/.test(cons);"
        "/[^a-z]/.test(slice);"
        "/[^a-z]/.test(slice_on_cons);");
    const char* expected_cons =
        "Now is the time for all good men to come to the aid of the party"
        "Now is the time for all good men to come to the aid of the party";
    const char* expected_slice =
        "ow is the time for all good men to come to the aid of the part";
    const char* expected_slice_on_cons =
        "ow is the time for all good men to come to the aid of the party"
        "Now is the time for all good men to come to the aid of the part";
    CHECK(String::NewFromUtf8(env->GetIsolate(), expected_cons)
              ->Equals(env->Global()->Get(v8_str("cons"))));
    CHECK(String::NewFromUtf8(env->GetIsolate(), expected_slice)
              ->Equals(env->Global()->Get(v8_str("slice"))));
    CHECK(String::NewFromUtf8(env->GetIsolate(), expected_slice_on_cons)
              ->Equals(env->Global()->Get(v8_str("slice_on_cons"))));
  }
  i::DeleteArray(two_byte_string);
}


TEST(CompileExternalTwoByteSource) {
  LocalContext context;
  v8::HandleScope scope(context->GetIsolate());

  // This is a very short list of sources, which currently is to check for a
  // regression caused by r2703.
  const char* one_byte_sources[] = {
      "0.5",
      "-0.5",   // This mainly testes PushBack in the Scanner.
      "--0.5",  // This mainly testes PushBack in the Scanner.
      NULL};

  // Compile the sources as external two byte strings.
  for (int i = 0; one_byte_sources[i] != NULL; i++) {
    uint16_t* two_byte_string = AsciiToTwoByteString(one_byte_sources[i]);
    TestResource* uc16_resource = new TestResource(two_byte_string);
    v8::Local<v8::String> source =
        v8::String::NewExternal(context->GetIsolate(), uc16_resource);
    v8::Script::Compile(source);
  }
}


#ifndef V8_INTERPRETED_REGEXP

struct RegExpInterruptionData {
  v8::base::Atomic32 loop_count;
  UC16VectorResource* string_resource;
  v8::Persistent<v8::String> string;
} regexp_interruption_data;


class RegExpInterruptionThread : public v8::base::Thread {
 public:
  explicit RegExpInterruptionThread(v8::Isolate* isolate)
      : Thread(Options("TimeoutThread")), isolate_(isolate) {}

  virtual void Run() {
    for (v8::base::NoBarrier_Store(&regexp_interruption_data.loop_count, 0);
         v8::base::NoBarrier_Load(&regexp_interruption_data.loop_count) < 7;
         v8::base::NoBarrier_AtomicIncrement(
             &regexp_interruption_data.loop_count, 1)) {
      v8::base::OS::Sleep(50);  // Wait a bit before requesting GC.
      reinterpret_cast<i::Isolate*>(isolate_)->stack_guard()->RequestGC();
    }
    v8::base::OS::Sleep(50);  // Wait a bit before terminating.
    v8::V8::TerminateExecution(isolate_);
  }

 private:
  v8::Isolate* isolate_;
};


void RunBeforeGC(v8::GCType type, v8::GCCallbackFlags flags) {
  if (v8::base::NoBarrier_Load(&regexp_interruption_data.loop_count) != 2) {
    return;
  }
  v8::HandleScope scope(CcTest::isolate());
  v8::Local<v8::String> string = v8::Local<v8::String>::New(
      CcTest::isolate(), regexp_interruption_data.string);
  string->MakeExternal(regexp_interruption_data.string_resource);
}


// Test that RegExp execution can be interrupted.  Specifically, we test
// * interrupting with GC
// * turn the subject string from one-byte internal to two-byte external string
// * force termination
TEST(RegExpInterruption) {
  v8::HandleScope scope(CcTest::isolate());
  LocalContext env;

  RegExpInterruptionThread timeout_thread(CcTest::isolate());

  v8::V8::AddGCPrologueCallback(RunBeforeGC);
  static const char* one_byte_content = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  i::uc16* uc16_content = AsciiToTwoByteString(one_byte_content);
  v8::Local<v8::String> string = v8_str(one_byte_content);

  CcTest::global()->Set(v8_str("a"), string);
  regexp_interruption_data.string.Reset(CcTest::isolate(), string);
  regexp_interruption_data.string_resource = new UC16VectorResource(
      i::Vector<const i::uc16>(uc16_content, i::StrLength(one_byte_content)));

  v8::TryCatch try_catch;
  timeout_thread.Start();

  CompileRun("/((a*)*)*b/.exec(a)");
  CHECK(try_catch.HasTerminated());

  timeout_thread.Join();

  regexp_interruption_data.string.Reset();
  i::DeleteArray(uc16_content);
}

#endif  // V8_INTERPRETED_REGEXP


// Test that we cannot set a property on the global object if there
// is a read-only property in the prototype chain.
TEST(ReadOnlyPropertyInGlobalProto) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  v8::Handle<v8::ObjectTemplate> templ = v8::ObjectTemplate::New(isolate);
  LocalContext context(0, templ);
  v8::Handle<v8::Object> global = context->Global();
  v8::Handle<v8::Object> global_proto =
      v8::Handle<v8::Object>::Cast(global->Get(v8_str("__proto__")));
  global_proto->ForceSet(v8_str("x"), v8::Integer::New(isolate, 0),
                         v8::ReadOnly);
  global_proto->ForceSet(v8_str("y"), v8::Integer::New(isolate, 0),
                         v8::ReadOnly);
  // Check without 'eval' or 'with'.
  v8::Handle<v8::Value> res =
      CompileRun("function f() { x = 42; return x; }; f()");
  CHECK(v8::Integer::New(isolate, 0)->Equals(res));
  // Check with 'eval'.
  res = CompileRun("function f() { eval('1'); y = 43; return y; }; f()");
  CHECK(v8::Integer::New(isolate, 0)->Equals(res));
  // Check with 'with'.
  res = CompileRun("function f() { with (this) { y = 44 }; return y; }; f()");
  CHECK(v8::Integer::New(isolate, 0)->Equals(res));
}

static int force_set_set_count = 0;
static int force_set_get_count = 0;
bool pass_on_get = false;

static void ForceSetGetter(v8::Local<v8::String> name,
                           const v8::PropertyCallbackInfo<v8::Value>& info) {
  force_set_get_count++;
  if (pass_on_get) {
    return;
  }
  info.GetReturnValue().Set(3);
}

static void ForceSetSetter(v8::Local<v8::String> name,
                           v8::Local<v8::Value> value,
                           const v8::PropertyCallbackInfo<void>& info) {
  force_set_set_count++;
}

static void ForceSetInterceptGetter(
    v8::Local<v8::Name> name, const v8::PropertyCallbackInfo<v8::Value>& info) {
  CHECK(name->IsString());
  ForceSetGetter(Local<String>::Cast(name), info);
}

static void ForceSetInterceptSetter(
    v8::Local<v8::Name> name, v8::Local<v8::Value> value,
    const v8::PropertyCallbackInfo<v8::Value>& info) {
  force_set_set_count++;
  info.GetReturnValue().SetUndefined();
}


TEST(ForceSet) {
  force_set_get_count = 0;
  force_set_set_count = 0;
  pass_on_get = false;

  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  v8::Handle<v8::ObjectTemplate> templ = v8::ObjectTemplate::New(isolate);
  v8::Handle<v8::String> access_property =
      v8::String::NewFromUtf8(isolate, "a");
  templ->SetAccessor(access_property, ForceSetGetter, ForceSetSetter);
  LocalContext context(NULL, templ);
  v8::Handle<v8::Object> global = context->Global();

  // Ordinary properties
  v8::Handle<v8::String> simple_property =
      v8::String::NewFromUtf8(isolate, "p");
  global->ForceSet(simple_property, v8::Int32::New(isolate, 4), v8::ReadOnly);
  CHECK_EQ(4, global->Get(simple_property)->Int32Value());
  // This should fail because the property is read-only
  global->Set(simple_property, v8::Int32::New(isolate, 5));
  CHECK_EQ(4, global->Get(simple_property)->Int32Value());
  // This should succeed even though the property is read-only
  global->ForceSet(simple_property, v8::Int32::New(isolate, 6));
  CHECK_EQ(6, global->Get(simple_property)->Int32Value());

  // Accessors
  CHECK_EQ(0, force_set_set_count);
  CHECK_EQ(0, force_set_get_count);
  CHECK_EQ(3, global->Get(access_property)->Int32Value());
  // CHECK_EQ the property shouldn't override it, just call the setter
  // which in this case does nothing.
  global->Set(access_property, v8::Int32::New(isolate, 7));
  CHECK_EQ(3, global->Get(access_property)->Int32Value());
  CHECK_EQ(1, force_set_set_count);
  CHECK_EQ(2, force_set_get_count);
  // Forcing the property to be set should override the accessor without
  // calling it
  global->ForceSet(access_property, v8::Int32::New(isolate, 8));
  CHECK_EQ(8, global->Get(access_property)->Int32Value());
  CHECK_EQ(1, force_set_set_count);
  CHECK_EQ(2, force_set_get_count);
}


TEST(ForceSetWithInterceptor) {
  force_set_get_count = 0;
  force_set_set_count = 0;
  pass_on_get = false;

  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  v8::Handle<v8::ObjectTemplate> templ = v8::ObjectTemplate::New(isolate);
  templ->SetHandler(v8::NamedPropertyHandlerConfiguration(
      ForceSetInterceptGetter, ForceSetInterceptSetter));
  LocalContext context(NULL, templ);
  v8::Handle<v8::Object> global = context->Global();

  v8::Handle<v8::String> some_property =
      v8::String::NewFromUtf8(isolate, "a");
  CHECK_EQ(0, force_set_set_count);
  CHECK_EQ(0, force_set_get_count);
  CHECK_EQ(3, global->Get(some_property)->Int32Value());
  // Setting the property shouldn't override it, just call the setter
  // which in this case does nothing.
  global->Set(some_property, v8::Int32::New(isolate, 7));
  CHECK_EQ(3, global->Get(some_property)->Int32Value());
  CHECK_EQ(1, force_set_set_count);
  CHECK_EQ(2, force_set_get_count);
  // Getting the property when the interceptor returns an empty handle
  // should yield undefined, since the property isn't present on the
  // object itself yet.
  pass_on_get = true;
  CHECK(global->Get(some_property)->IsUndefined());
  CHECK_EQ(1, force_set_set_count);
  CHECK_EQ(3, force_set_get_count);
  // Forcing the property to be set should cause the value to be
  // set locally without calling the interceptor.
  global->ForceSet(some_property, v8::Int32::New(isolate, 8));
  CHECK_EQ(8, global->Get(some_property)->Int32Value());
  CHECK_EQ(1, force_set_set_count);
  CHECK_EQ(4, force_set_get_count);
  // Reenabling the interceptor should cause it to take precedence over
  // the property
  pass_on_get = false;
  CHECK_EQ(3, global->Get(some_property)->Int32Value());
  CHECK_EQ(1, force_set_set_count);
  CHECK_EQ(5, force_set_get_count);
  // The interceptor should also work for other properties
  CHECK_EQ(3, global->Get(v8::String::NewFromUtf8(isolate, "b"))
                  ->Int32Value());
  CHECK_EQ(1, force_set_set_count);
  CHECK_EQ(6, force_set_get_count);
}


static v8::Local<Context> calling_context0;
static v8::Local<Context> calling_context1;
static v8::Local<Context> calling_context2;


// Check that the call to the callback is initiated in
// calling_context2, the directly calling context is calling_context1
// and the callback itself is in calling_context0.
static void GetCallingContextCallback(
    const v8::FunctionCallbackInfo<v8::Value>& args) {
  ApiTestFuzzer::Fuzz();
  CHECK(args.GetIsolate()->GetCurrentContext() == calling_context0);
  CHECK(args.GetIsolate()->GetCallingContext() == calling_context1);
  CHECK(args.GetIsolate()->GetEnteredContext() == calling_context2);
  args.GetReturnValue().Set(42);
}


THREADED_TEST(GetCurrentContextWhenNotInContext) {
  i::Isolate* isolate = CcTest::i_isolate();
  CHECK(isolate != NULL);
  CHECK(isolate->context() == NULL);
  v8::Isolate* v8_isolate = reinterpret_cast<v8::Isolate*>(isolate);
  v8::HandleScope scope(v8_isolate);
  // The following should not crash, but return an empty handle.
  v8::Local<v8::Context> current = v8_isolate->GetCurrentContext();
  CHECK(current.IsEmpty());
}


THREADED_TEST(GetCallingContext) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);

  Local<Context> calling_context0(Context::New(isolate));
  Local<Context> calling_context1(Context::New(isolate));
  Local<Context> calling_context2(Context::New(isolate));
  ::calling_context0 = calling_context0;
  ::calling_context1 = calling_context1;
  ::calling_context2 = calling_context2;

  // Allow cross-domain access.
  Local<String> token = v8_str("<security token>");
  calling_context0->SetSecurityToken(token);
  calling_context1->SetSecurityToken(token);
  calling_context2->SetSecurityToken(token);

  // Create an object with a C++ callback in context0.
  calling_context0->Enter();
  Local<v8::FunctionTemplate> callback_templ =
      v8::FunctionTemplate::New(isolate, GetCallingContextCallback);
  calling_context0->Global()->Set(v8_str("callback"),
                                  callback_templ->GetFunction());
  calling_context0->Exit();

  // Expose context0 in context1 and set up a function that calls the
  // callback function.
  calling_context1->Enter();
  calling_context1->Global()->Set(v8_str("context0"),
                                  calling_context0->Global());
  CompileRun("function f() { context0.callback() }");
  calling_context1->Exit();

  // Expose context1 in context2 and call the callback function in
  // context0 indirectly through f in context1.
  calling_context2->Enter();
  calling_context2->Global()->Set(v8_str("context1"),
                                  calling_context1->Global());
  CompileRun("context1.f()");
  calling_context2->Exit();
  ::calling_context0.Clear();
  ::calling_context1.Clear();
  ::calling_context2.Clear();
}


// Check that a variable declaration with no explicit initialization
// value does shadow an existing property in the prototype chain.
THREADED_TEST(InitGlobalVarInProtoChain) {
  LocalContext context;
  v8::HandleScope scope(context->GetIsolate());
  // Introduce a variable in the prototype chain.
  CompileRun("__proto__.x = 42");
  v8::Handle<v8::Value> result = CompileRun("var x = 43; x");
  CHECK(!result->IsUndefined());
  CHECK_EQ(43, result->Int32Value());
}


// Regression test for issue 398.
// If a function is added to an object, creating a constant function
// field, and the result is cloned, replacing the constant function on the
// original should not affect the clone.
// See http://code.google.com/p/v8/issues/detail?id=398
THREADED_TEST(ReplaceConstantFunction) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);
  v8::Handle<v8::Object> obj = v8::Object::New(isolate);
  v8::Handle<v8::FunctionTemplate> func_templ =
      v8::FunctionTemplate::New(isolate);
  v8::Handle<v8::String> foo_string =
      v8::String::NewFromUtf8(isolate, "foo");
  obj->Set(foo_string, func_templ->GetFunction());
  v8::Handle<v8::Object> obj_clone = obj->Clone();
  obj_clone->Set(foo_string,
                 v8::String::NewFromUtf8(isolate, "Hello"));
  CHECK(!obj->Get(foo_string)->IsUndefined());
}


static void CheckElementValue(i::Isolate* isolate,
                              int expected,
                              i::Handle<i::Object> obj,
                              int offset) {
  i::Object* element =
      *i::Object::GetElement(isolate, obj, offset).ToHandleChecked();
  CHECK_EQ(expected, i::Smi::cast(element)->value());
}


THREADED_TEST(PixelArray) {
  LocalContext context;
  i::Isolate* isolate = CcTest::i_isolate();
  i::Factory* factory = isolate->factory();
  v8::HandleScope scope(context->GetIsolate());
  const int kElementCount = 260;
  uint8_t* pixel_data = reinterpret_cast<uint8_t*>(malloc(kElementCount));
  i::Handle<i::ExternalUint8ClampedArray> pixels =
      i::Handle<i::ExternalUint8ClampedArray>::cast(
          factory->NewExternalArray(kElementCount,
                                    v8::kExternalUint8ClampedArray,
                                    pixel_data));
  // Force GC to trigger verification.
  CcTest::heap()->CollectAllGarbage(i::Heap::kNoGCFlags);
  for (int i = 0; i < kElementCount; i++) {
    pixels->set(i, i % 256);
  }
  // Force GC to trigger verification.
  CcTest::heap()->CollectAllGarbage(i::Heap::kNoGCFlags);
  for (int i = 0; i < kElementCount; i++) {
    CHECK_EQ(i % 256, pixels->get_scalar(i));
    CHECK_EQ(i % 256, pixel_data[i]);
  }

  v8::Handle<v8::Object> obj = v8::Object::New(context->GetIsolate());
  i::Handle<i::JSObject> jsobj = v8::Utils::OpenHandle(*obj);
  // Set the elements to be the pixels.
  // jsobj->set_elements(*pixels);
  obj->SetIndexedPropertiesToPixelData(pixel_data, kElementCount);
  CheckElementValue(isolate, 1, jsobj, 1);
  obj->Set(v8_str("field"), v8::Int32::New(CcTest::isolate(), 1503));
  context->Global()->Set(v8_str("pixels"), obj);
  v8::Handle<v8::Value> result = CompileRun("pixels.field");
  CHECK_EQ(1503, result->Int32Value());
  result = CompileRun("pixels[1]");
  CHECK_EQ(1, result->Int32Value());

  result = CompileRun("var sum = 0;"
                      "for (var i = 0; i < 8; i++) {"
                      "  sum += pixels[i] = pixels[i] = -i;"
                      "}"
                      "sum;");
  CHECK_EQ(-28, result->Int32Value());

  result = CompileRun("var sum = 0;"
                      "for (var i = 0; i < 8; i++) {"
                      "  sum += pixels[i] = pixels[i] = 0;"
                      "}"
                      "sum;");
  CHECK_EQ(0, result->Int32Value());

  result = CompileRun("var sum = 0;"
                      "for (var i = 0; i < 8; i++) {"
                      "  sum += pixels[i] = pixels[i] = 255;"
                      "}"
                      "sum;");
  CHECK_EQ(8 * 255, result->Int32Value());

  result = CompileRun("var sum = 0;"
                      "for (var i = 0; i < 8; i++) {"
                      "  sum += pixels[i] = pixels[i] = 256 + i;"
                      "}"
                      "sum;");
  CHECK_EQ(2076, result->Int32Value());

  result = CompileRun("var sum = 0;"
                      "for (var i = 0; i < 8; i++) {"
                      "  sum += pixels[i] = pixels[i] = i;"
                      "}"
                      "sum;");
  CHECK_EQ(28, result->Int32Value());

  result = CompileRun("var sum = 0;"
                      "for (var i = 0; i < 8; i++) {"
                      "  sum += pixels[i];"
                      "}"
                      "sum;");
  CHECK_EQ(28, result->Int32Value());

  i::Handle<i::Smi> value(i::Smi::FromInt(2),
                          reinterpret_cast<i::Isolate*>(context->GetIsolate()));
  i::Handle<i::Object> no_failure;
  no_failure = i::JSObject::SetElement(
      jsobj, 1, value, NONE, i::SLOPPY).ToHandleChecked();
  DCHECK(!no_failure.is_null());
  USE(no_failure);
  CheckElementValue(isolate, 2, jsobj, 1);
  *value.location() = i::Smi::FromInt(256);
  no_failure = i::JSObject::SetElement(
      jsobj, 1, value, NONE, i::SLOPPY).ToHandleChecked();
  DCHECK(!no_failure.is_null());
  USE(no_failure);
  CheckElementValue(isolate, 255, jsobj, 1);
  *value.location() = i::Smi::FromInt(-1);
  no_failure = i::JSObject::SetElement(
      jsobj, 1, value, NONE, i::SLOPPY).ToHandleChecked();
  DCHECK(!no_failure.is_null());
  USE(no_failure);
  CheckElementValue(isolate, 0, jsobj, 1);

  result = CompileRun("for (var i = 0; i < 8; i++) {"
                      "  pixels[i] = (i * 65) - 109;"
                      "}"
                      "pixels[1] + pixels[6];");
  CHECK_EQ(255, result->Int32Value());
  CheckElementValue(isolate, 0, jsobj, 0);
  CheckElementValue(isolate, 0, jsobj, 1);
  CheckElementValue(isolate, 21, jsobj, 2);
  CheckElementValue(isolate, 86, jsobj, 3);
  CheckElementValue(isolate, 151, jsobj, 4);
  CheckElementValue(isolate, 216, jsobj, 5);
  CheckElementValue(isolate, 255, jsobj, 6);
  CheckElementValue(isolate, 255, jsobj, 7);
  result = CompileRun("var sum = 0;"
                      "for (var i = 0; i < 8; i++) {"
                      "  sum += pixels[i];"
                      "}"
                      "sum;");
  CHECK_EQ(984, result->Int32Value());

  result = CompileRun("for (var i = 0; i < 8; i++) {"
                      "  pixels[i] = (i * 1.1);"
                      "}"
                      "pixels[1] + pixels[6];");
  CHECK_EQ(8, result->Int32Value());
  CheckElementValue(isolate, 0, jsobj, 0);
  CheckElementValue(isolate, 1, jsobj, 1);
  CheckElementValue(isolate, 2, jsobj, 2);
  CheckElementValue(isolate, 3, jsobj, 3);
  CheckElementValue(isolate, 4, jsobj, 4);
  CheckElementValue(isolate, 6, jsobj, 5);
  CheckElementValue(isolate, 7, jsobj, 6);
  CheckElementValue(isolate, 8, jsobj, 7);

  result = CompileRun("for (var i = 0; i < 8; i++) {"
                      "  pixels[7] = undefined;"
                      "}"
                      "pixels[7];");
  CHECK_EQ(0, result->Int32Value());
  CheckElementValue(isolate, 0, jsobj, 7);

  result = CompileRun("for (var i = 0; i < 8; i++) {"
                      "  pixels[6] = '2.3';"
                      "}"
                      "pixels[6];");
  CHECK_EQ(2, result->Int32Value());
  CheckElementValue(isolate, 2, jsobj, 6);

  result = CompileRun("for (var i = 0; i < 8; i++) {"
                      "  pixels[5] = NaN;"
                      "}"
                      "pixels[5];");
  CHECK_EQ(0, result->Int32Value());
  CheckElementValue(isolate, 0, jsobj, 5);

  result = CompileRun("for (var i = 0; i < 8; i++) {"
                      "  pixels[8] = Infinity;"
                      "}"
                      "pixels[8];");
  CHECK_EQ(255, result->Int32Value());
  CheckElementValue(isolate, 255, jsobj, 8);

  result = CompileRun("for (var i = 0; i < 8; i++) {"
                      "  pixels[9] = -Infinity;"
                      "}"
                      "pixels[9];");
  CHECK_EQ(0, result->Int32Value());
  CheckElementValue(isolate, 0, jsobj, 9);

  result = CompileRun("pixels[3] = 33;"
                      "delete pixels[3];"
                      "pixels[3];");
  CHECK_EQ(33, result->Int32Value());

  result = CompileRun("pixels[0] = 10; pixels[1] = 11;"
                      "pixels[2] = 12; pixels[3] = 13;"
                      "pixels.__defineGetter__('2',"
                      "function() { return 120; });"
                      "pixels[2];");
  CHECK_EQ(12, result->Int32Value());

  result = CompileRun("var js_array = new Array(40);"
                      "js_array[0] = 77;"
                      "js_array;");
  CHECK_EQ(77, v8::Object::Cast(*result)->Get(v8_str("0"))->Int32Value());

  result = CompileRun("pixels[1] = 23;"
                      "pixels.__proto__ = [];"
                      "js_array.__proto__ = pixels;"
                      "js_array.concat(pixels);");
  CHECK_EQ(77, v8::Object::Cast(*result)->Get(v8_str("0"))->Int32Value());
  CHECK_EQ(23, v8::Object::Cast(*result)->Get(v8_str("1"))->Int32Value());

  result = CompileRun("pixels[1] = 23;");
  CHECK_EQ(23, result->Int32Value());

  // Test for index greater than 255.  Regression test for:
  // http://code.google.com/p/chromium/issues/detail?id=26337.
  result = CompileRun("pixels[256] = 255;");
  CHECK_EQ(255, result->Int32Value());
  result = CompileRun("var i = 0;"
                      "for (var j = 0; j < 8; j++) { i = pixels[256]; }"
                      "i");
  CHECK_EQ(255, result->Int32Value());

  // Make sure that pixel array ICs recognize when a non-pixel array
  // is passed to it.
  result = CompileRun("function pa_load(p) {"
                      "  var sum = 0;"
                      "  for (var j = 0; j < 256; j++) { sum += p[j]; }"
                      "  return sum;"
                      "}"
                      "for (var i = 0; i < 256; ++i) { pixels[i] = i; }"
                      "for (var i = 0; i < 10; ++i) { pa_load(pixels); }"
                      "just_ints = new Object();"
                      "for (var i = 0; i < 256; ++i) { just_ints[i] = i; }"
                      "for (var i = 0; i < 10; ++i) {"
                      "  result = pa_load(just_ints);"
                      "}"
                      "result");
  CHECK_EQ(32640, result->Int32Value());

  // Make sure that pixel array ICs recognize out-of-bound accesses.
  result = CompileRun("function pa_load(p, start) {"
                      "  var sum = 0;"
                      "  for (var j = start; j < 256; j++) { sum += p[j]; }"
                      "  return sum;"
                      "}"
                      "for (var i = 0; i < 256; ++i) { pixels[i] = i; }"
                      "for (var i = 0; i < 10; ++i) { pa_load(pixels,0); }"
                      "for (var i = 0; i < 10; ++i) {"
                      "  result = pa_load(pixels,-10);"
                      "}"
                      "result");
  CHECK_EQ(0, result->Int32Value());

  // Make sure that generic ICs properly handles a pixel array.
  result = CompileRun("function pa_load(p) {"
                      "  var sum = 0;"
                      "  for (var j = 0; j < 256; j++) { sum += p[j]; }"
                      "  return sum;"
                      "}"
                      "for (var i = 0; i < 256; ++i) { pixels[i] = i; }"
                      "just_ints = new Object();"
                      "for (var i = 0; i < 256; ++i) { just_ints[i] = i; }"
                      "for (var i = 0; i < 10; ++i) { pa_load(just_ints); }"
                      "for (var i = 0; i < 10; ++i) {"
                      "  result = pa_load(pixels);"
                      "}"
                      "result");
  CHECK_EQ(32640, result->Int32Value());

  // Make sure that generic load ICs recognize out-of-bound accesses in
  // pixel arrays.
  result = CompileRun("function pa_load(p, start) {"
                      "  var sum = 0;"
                      "  for (var j = start; j < 256; j++) { sum += p[j]; }"
                      "  return sum;"
                      "}"
                      "for (var i = 0; i < 256; ++i) { pixels[i] = i; }"
                      "just_ints = new Object();"
                      "for (var i = 0; i < 256; ++i) { just_ints[i] = i; }"
                      "for (var i = 0; i < 10; ++i) { pa_load(just_ints,0); }"
                      "for (var i = 0; i < 10; ++i) { pa_load(pixels,0); }"
                      "for (var i = 0; i < 10; ++i) {"
                      "  result = pa_load(pixels,-10);"
                      "}"
                      "result");
  CHECK_EQ(0, result->Int32Value());

  // Make sure that generic ICs properly handles other types than pixel
  // arrays (that the inlined fast pixel array test leaves the right information
  // in the right registers).
  result = CompileRun("function pa_load(p) {"
                      "  var sum = 0;"
                      "  for (var j = 0; j < 256; j++) { sum += p[j]; }"
                      "  return sum;"
                      "}"
                      "for (var i = 0; i < 256; ++i) { pixels[i] = i; }"
                      "just_ints = new Object();"
                      "for (var i = 0; i < 256; ++i) { just_ints[i] = i; }"
                      "for (var i = 0; i < 10; ++i) { pa_load(just_ints); }"
                      "for (var i = 0; i < 10; ++i) { pa_load(pixels); }"
                      "sparse_array = new Object();"
                      "for (var i = 0; i < 256; ++i) { sparse_array[i] = i; }"
                      "sparse_array[1000000] = 3;"
                      "for (var i = 0; i < 10; ++i) {"
                      "  result = pa_load(sparse_array);"
                      "}"
                      "result");
  CHECK_EQ(32640, result->Int32Value());

  // Make sure that pixel array store ICs clamp values correctly.
  result = CompileRun("function pa_store(p) {"
                      "  for (var j = 0; j < 256; j++) { p[j] = j * 2; }"
                      "}"
                      "pa_store(pixels);"
                      "var sum = 0;"
                      "for (var j = 0; j < 256; j++) { sum += pixels[j]; }"
                      "sum");
  CHECK_EQ(48896, result->Int32Value());

  // Make sure that pixel array stores correctly handle accesses outside
  // of the pixel array..
  result = CompileRun("function pa_store(p,start) {"
                      "  for (var j = 0; j < 256; j++) {"
                      "    p[j+start] = j * 2;"
                      "  }"
                      "}"
                      "pa_store(pixels,0);"
                      "pa_store(pixels,-128);"
                      "var sum = 0;"
                      "for (var j = 0; j < 256; j++) { sum += pixels[j]; }"
                      "sum");
  CHECK_EQ(65280, result->Int32Value());

  // Make sure that the generic store stub correctly handle accesses outside
  // of the pixel array..
  result = CompileRun("function pa_store(p,start) {"
                      "  for (var j = 0; j < 256; j++) {"
                      "    p[j+start] = j * 2;"
                      "  }"
                      "}"
                      "pa_store(pixels,0);"
                      "just_ints = new Object();"
                      "for (var i = 0; i < 256; ++i) { just_ints[i] = i; }"
                      "pa_store(just_ints, 0);"
                      "pa_store(pixels,-128);"
                      "var sum = 0;"
                      "for (var j = 0; j < 256; j++) { sum += pixels[j]; }"
                      "sum");
  CHECK_EQ(65280, result->Int32Value());

  // Make sure that the generic keyed store stub clamps pixel array values
  // correctly.
  result = CompileRun("function pa_store(p) {"
                      "  for (var j = 0; j < 256; j++) { p[j] = j * 2; }"
                      "}"
                      "pa_store(pixels);"
                      "just_ints = new Object();"
                      "pa_store(just_ints);"
                      "pa_store(pixels);"
                      "var sum = 0;"
                      "for (var j = 0; j < 256; j++) { sum += pixels[j]; }"
                      "sum");
  CHECK_EQ(48896, result->Int32Value());

  // Make sure that pixel array loads are optimized by crankshaft.
  result = CompileRun("function pa_load(p) {"
                      "  var sum = 0;"
                      "  for (var i=0; i<256; ++i) {"
                      "    sum += p[i];"
                      "  }"
                      "  return sum; "
                      "}"
                      "for (var i = 0; i < 256; ++i) { pixels[i] = i; }"
                      "for (var i = 0; i < 5000; ++i) {"
                      "  result = pa_load(pixels);"
                      "}"
                      "result");
  CHECK_EQ(32640, result->Int32Value());

  // Make sure that pixel array stores are optimized by crankshaft.
  result = CompileRun("function pa_init(p) {"
                      "for (var i = 0; i < 256; ++i) { p[i] = i; }"
                      "}"
                      "function pa_load(p) {"
                      "  var sum = 0;"
                      "  for (var i=0; i<256; ++i) {"
                      "    sum += p[i];"
                      "  }"
                      "  return sum; "
                      "}"
                      "for (var i = 0; i < 5000; ++i) {"
                      "  pa_init(pixels);"
                      "}"
                      "result = pa_load(pixels);"
                      "result");
  CHECK_EQ(32640, result->Int32Value());

  free(pixel_data);
}


THREADED_TEST(PixelArrayInfo) {
  LocalContext context;
  v8::HandleScope scope(context->GetIsolate());
  for (int size = 0; size < 100; size += 10) {
    uint8_t* pixel_data = reinterpret_cast<uint8_t*>(malloc(size));
    v8::Handle<v8::Object> obj = v8::Object::New(context->GetIsolate());
    obj->SetIndexedPropertiesToPixelData(pixel_data, size);
    CHECK(obj->HasIndexedPropertiesInPixelData());
    CHECK_EQ(pixel_data, obj->GetIndexedPropertiesPixelData());
    CHECK_EQ(size, obj->GetIndexedPropertiesPixelDataLength());
    free(pixel_data);
  }
}


static void NotHandledIndexedPropertyGetter(
    uint32_t index,
    const v8::PropertyCallbackInfo<v8::Value>& info) {
  ApiTestFuzzer::Fuzz();
}


static void NotHandledIndexedPropertySetter(
    uint32_t index,
    Local<Value> value,
    const v8::PropertyCallbackInfo<v8::Value>& info) {
  ApiTestFuzzer::Fuzz();
}


THREADED_TEST(PixelArrayWithInterceptor) {
  LocalContext context;
  i::Factory* factory = CcTest::i_isolate()->factory();
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);
  const int kElementCount = 260;
  uint8_t* pixel_data = reinterpret_cast<uint8_t*>(malloc(kElementCount));
  i::Handle<i::ExternalUint8ClampedArray> pixels =
      i::Handle<i::ExternalUint8ClampedArray>::cast(
          factory->NewExternalArray(kElementCount,
                                    v8::kExternalUint8ClampedArray,
                                    pixel_data));
  for (int i = 0; i < kElementCount; i++) {
    pixels->set(i, i % 256);
  }
  v8::Handle<v8::ObjectTemplate> templ =
      v8::ObjectTemplate::New(context->GetIsolate());
  templ->SetHandler(v8::IndexedPropertyHandlerConfiguration(
      NotHandledIndexedPropertyGetter, NotHandledIndexedPropertySetter));
  v8::Handle<v8::Object> obj = templ->NewInstance();
  obj->SetIndexedPropertiesToPixelData(pixel_data, kElementCount);
  context->Global()->Set(v8_str("pixels"), obj);
  v8::Handle<v8::Value> result = CompileRun("pixels[1]");
  CHECK_EQ(1, result->Int32Value());
  result = CompileRun("var sum = 0;"
                      "for (var i = 0; i < 8; i++) {"
                      "  sum += pixels[i] = pixels[i] = -i;"
                      "}"
                      "sum;");
  CHECK_EQ(-28, result->Int32Value());
  result = CompileRun("pixels.hasOwnProperty('1')");
  CHECK(result->BooleanValue());
  free(pixel_data);
}


static int ExternalArrayElementSize(v8::ExternalArrayType array_type) {
  switch (array_type) {
    case v8::kExternalInt8Array:
    case v8::kExternalUint8Array:
    case v8::kExternalUint8ClampedArray:
      return 1;
      break;
    case v8::kExternalInt16Array:
    case v8::kExternalUint16Array:
      return 2;
      break;
    case v8::kExternalInt32Array:
    case v8::kExternalUint32Array:
    case v8::kExternalFloat32Array:
      return 4;
      break;
    case v8::kExternalFloat64Array:
      return 8;
      break;
    default:
      UNREACHABLE();
      return -1;
  }
  UNREACHABLE();
  return -1;
}


template <class ExternalArrayClass, class ElementType>
static void ObjectWithExternalArrayTestHelper(
    Handle<Context> context,
    v8::Handle<Object> obj,
    int element_count,
    v8::ExternalArrayType array_type,
    int64_t low, int64_t high) {
  i::Handle<i::JSObject> jsobj = v8::Utils::OpenHandle(*obj);
  i::Isolate* isolate = jsobj->GetIsolate();
  obj->Set(v8_str("field"),
           v8::Int32::New(reinterpret_cast<v8::Isolate*>(isolate), 1503));
  context->Global()->Set(v8_str("ext_array"), obj);
  v8::Handle<v8::Value> result = CompileRun("ext_array.field");
  CHECK_EQ(1503, result->Int32Value());
  result = CompileRun("ext_array[1]");
  CHECK_EQ(1, result->Int32Value());

  // Check assigned smis
  result = CompileRun("for (var i = 0; i < 8; i++) {"
                      "  ext_array[i] = i;"
                      "}"
                      "var sum = 0;"
                      "for (var i = 0; i < 8; i++) {"
                      "  sum += ext_array[i];"
                      "}"
                      "sum;");

  CHECK_EQ(28, result->Int32Value());
  // Check pass through of assigned smis
  result = CompileRun("var sum = 0;"
                      "for (var i = 0; i < 8; i++) {"
                      "  sum += ext_array[i] = ext_array[i] = -i;"
                      "}"
                      "sum;");
  CHECK_EQ(-28, result->Int32Value());


  // Check assigned smis in reverse order
  result = CompileRun("for (var i = 8; --i >= 0; ) {"
                      "  ext_array[i] = i;"
                      "}"
                      "var sum = 0;"
                      "for (var i = 0; i < 8; i++) {"
                      "  sum += ext_array[i];"
                      "}"
                      "sum;");
  CHECK_EQ(28, result->Int32Value());

  // Check pass through of assigned HeapNumbers
  result = CompileRun("var sum = 0;"
                      "for (var i = 0; i < 16; i+=2) {"
                      "  sum += ext_array[i] = ext_array[i] = (-i * 0.5);"
                      "}"
                      "sum;");
  CHECK_EQ(-28, result->Int32Value());

  // Check assigned HeapNumbers
  result = CompileRun("for (var i = 0; i < 16; i+=2) {"
                      "  ext_array[i] = (i * 0.5);"
                      "}"
                      "var sum = 0;"
                      "for (var i = 0; i < 16; i+=2) {"
                      "  sum += ext_array[i];"
                      "}"
                      "sum;");
  CHECK_EQ(28, result->Int32Value());

  // Check assigned HeapNumbers in reverse order
  result = CompileRun("for (var i = 14; i >= 0; i-=2) {"
                      "  ext_array[i] = (i * 0.5);"
                      "}"
                      "var sum = 0;"
                      "for (var i = 0; i < 16; i+=2) {"
                      "  sum += ext_array[i];"
                      "}"
                      "sum;");
  CHECK_EQ(28, result->Int32Value());

  i::ScopedVector<char> test_buf(1024);

  // Check legal boundary conditions.
  // The repeated loads and stores ensure the ICs are exercised.
  const char* boundary_program =
      "var res = 0;"
      "for (var i = 0; i < 16; i++) {"
      "  ext_array[i] = %lld;"
      "  if (i > 8) {"
      "    res = ext_array[i];"
      "  }"
      "}"
      "res;";
  i::SNPrintF(test_buf,
              boundary_program,
              low);
  result = CompileRun(test_buf.start());
  CHECK_EQ(low, result->IntegerValue());

  i::SNPrintF(test_buf,
              boundary_program,
              high);
  result = CompileRun(test_buf.start());
  CHECK_EQ(high, result->IntegerValue());

  // Check misprediction of type in IC.
  result = CompileRun("var tmp_array = ext_array;"
                      "var sum = 0;"
                      "for (var i = 0; i < 8; i++) {"
                      "  tmp_array[i] = i;"
                      "  sum += tmp_array[i];"
                      "  if (i == 4) {"
                      "    tmp_array = {};"
                      "  }"
                      "}"
                      "sum;");
  // Force GC to trigger verification.
  CcTest::heap()->CollectAllGarbage(i::Heap::kNoGCFlags);
  CHECK_EQ(28, result->Int32Value());

  // Make sure out-of-range loads do not throw.
  i::SNPrintF(test_buf,
              "var caught_exception = false;"
              "try {"
              "  ext_array[%d];"
              "} catch (e) {"
              "  caught_exception = true;"
              "}"
              "caught_exception;",
              element_count);
  result = CompileRun(test_buf.start());
  CHECK_EQ(false, result->BooleanValue());

  // Make sure out-of-range stores do not throw.
  i::SNPrintF(test_buf,
              "var caught_exception = false;"
              "try {"
              "  ext_array[%d] = 1;"
              "} catch (e) {"
              "  caught_exception = true;"
              "}"
              "caught_exception;",
              element_count);
  result = CompileRun(test_buf.start());
  CHECK_EQ(false, result->BooleanValue());

  // Check other boundary conditions, values and operations.
  result = CompileRun("for (var i = 0; i < 8; i++) {"
                      "  ext_array[7] = undefined;"
                      "}"
                      "ext_array[7];");
  CHECK_EQ(0, result->Int32Value());
  if (array_type == v8::kExternalFloat64Array ||
      array_type == v8::kExternalFloat32Array) {
    CHECK(std::isnan(
        i::Object::GetElement(isolate, jsobj, 7).ToHandleChecked()->Number()));
  } else {
    CheckElementValue(isolate, 0, jsobj, 7);
  }

  result = CompileRun("for (var i = 0; i < 8; i++) {"
                      "  ext_array[6] = '2.3';"
                      "}"
                      "ext_array[6];");
  CHECK_EQ(2, result->Int32Value());
  CHECK_EQ(2,
           static_cast<int>(
               i::Object::GetElement(
                   isolate, jsobj, 6).ToHandleChecked()->Number()));

  if (array_type != v8::kExternalFloat32Array &&
      array_type != v8::kExternalFloat64Array) {
    // Though the specification doesn't state it, be explicit about
    // converting NaNs and +/-Infinity to zero.
    result = CompileRun("for (var i = 0; i < 8; i++) {"
                        "  ext_array[i] = 5;"
                        "}"
                        "for (var i = 0; i < 8; i++) {"
                        "  ext_array[i] = NaN;"
                        "}"
                        "ext_array[5];");
    CHECK_EQ(0, result->Int32Value());
    CheckElementValue(isolate, 0, jsobj, 5);

    result = CompileRun("for (var i = 0; i < 8; i++) {"
                        "  ext_array[i] = 5;"
                        "}"
                        "for (var i = 0; i < 8; i++) {"
                        "  ext_array[i] = Infinity;"
                        "}"
                        "ext_array[5];");
    int expected_value =
        (array_type == v8::kExternalUint8ClampedArray) ? 255 : 0;
    CHECK_EQ(expected_value, result->Int32Value());
    CheckElementValue(isolate, expected_value, jsobj, 5);

    result = CompileRun("for (var i = 0; i < 8; i++) {"
                        "  ext_array[i] = 5;"
                        "}"
                        "for (var i = 0; i < 8; i++) {"
                        "  ext_array[i] = -Infinity;"
                        "}"
                        "ext_array[5];");
    CHECK_EQ(0, result->Int32Value());
    CheckElementValue(isolate, 0, jsobj, 5);

    // Check truncation behavior of integral arrays.
    const char* unsigned_data =
        "var source_data = [0.6, 10.6];"
        "var expected_results = [0, 10];";
    const char* signed_data =
        "var source_data = [0.6, 10.6, -0.6, -10.6];"
        "var expected_results = [0, 10, 0, -10];";
    const char* pixel_data =
        "var source_data = [0.6, 10.6];"
        "var expected_results = [1, 11];";
    bool is_unsigned =
        (array_type == v8::kExternalUint8Array ||
         array_type == v8::kExternalUint16Array ||
         array_type == v8::kExternalUint32Array);
    bool is_pixel_data = array_type == v8::kExternalUint8ClampedArray;

    i::SNPrintF(test_buf,
                "%s"
                "var all_passed = true;"
                "for (var i = 0; i < source_data.length; i++) {"
                "  for (var j = 0; j < 8; j++) {"
                "    ext_array[j] = source_data[i];"
                "  }"
                "  all_passed = all_passed &&"
                "               (ext_array[5] == expected_results[i]);"
                "}"
                "all_passed;",
                (is_unsigned ?
                     unsigned_data :
                     (is_pixel_data ? pixel_data : signed_data)));
    result = CompileRun(test_buf.start());
    CHECK_EQ(true, result->BooleanValue());
  }

  i::Handle<ExternalArrayClass> array(
      ExternalArrayClass::cast(jsobj->elements()));
  for (int i = 0; i < element_count; i++) {
    array->set(i, static_cast<ElementType>(i));
  }

  // Test complex assignments
  result = CompileRun("function ee_op_test_complex_func(sum) {"
                      " for (var i = 0; i < 40; ++i) {"
                      "   sum += (ext_array[i] += 1);"
                      "   sum += (ext_array[i] -= 1);"
                      " } "
                      " return sum;"
                      "}"
                      "sum=0;"
                      "for (var i=0;i<10000;++i) {"
                      "  sum=ee_op_test_complex_func(sum);"
                      "}"
                      "sum;");
  CHECK_EQ(16000000, result->Int32Value());

  // Test count operations
  result = CompileRun("function ee_op_test_count_func(sum) {"
                      " for (var i = 0; i < 40; ++i) {"
                      "   sum += (++ext_array[i]);"
                      "   sum += (--ext_array[i]);"
                      " } "
                      " return sum;"
                      "}"
                      "sum=0;"
                      "for (var i=0;i<10000;++i) {"
                      "  sum=ee_op_test_count_func(sum);"
                      "}"
                      "sum;");
  CHECK_EQ(16000000, result->Int32Value());

  result = CompileRun("ext_array[3] = 33;"
                      "delete ext_array[3];"
                      "ext_array[3];");
  CHECK_EQ(33, result->Int32Value());

  result = CompileRun("ext_array[0] = 10; ext_array[1] = 11;"
                      "ext_array[2] = 12; ext_array[3] = 13;"
                      "ext_array.__defineGetter__('2',"
                      "function() { return 120; });"
                      "ext_array[2];");
  CHECK_EQ(12, result->Int32Value());

  result = CompileRun("var js_array = new Array(40);"
                      "js_array[0] = 77;"
                      "js_array;");
  CHECK_EQ(77, v8::Object::Cast(*result)->Get(v8_str("0"))->Int32Value());

  result = CompileRun("ext_array[1] = 23;"
                      "ext_array.__proto__ = [];"
                      "js_array.__proto__ = ext_array;"
                      "js_array.concat(ext_array);");
  CHECK_EQ(77, v8::Object::Cast(*result)->Get(v8_str("0"))->Int32Value());
  CHECK_EQ(23, v8::Object::Cast(*result)->Get(v8_str("1"))->Int32Value());

  result = CompileRun("ext_array[1] = 23;");
  CHECK_EQ(23, result->Int32Value());
}


template <class FixedTypedArrayClass,
          i::ElementsKind elements_kind,
          class ElementType>
static void FixedTypedArrayTestHelper(
    v8::ExternalArrayType array_type,
    ElementType low,
    ElementType high) {
  i::FLAG_allow_natives_syntax = true;
  LocalContext context;
  i::Isolate* isolate = CcTest::i_isolate();
  i::Factory* factory = isolate->factory();
  v8::HandleScope scope(context->GetIsolate());
  const int kElementCount = 260;
  i::Handle<FixedTypedArrayClass> fixed_array =
    i::Handle<FixedTypedArrayClass>::cast(
        factory->NewFixedTypedArray(kElementCount, array_type));
  CHECK_EQ(FixedTypedArrayClass::kInstanceType,
           fixed_array->map()->instance_type());
  CHECK_EQ(kElementCount, fixed_array->length());
  CcTest::heap()->CollectAllGarbage(i::Heap::kNoGCFlags);
  for (int i = 0; i < kElementCount; i++) {
    fixed_array->set(i, static_cast<ElementType>(i));
  }
  // Force GC to trigger verification.
  CcTest::heap()->CollectAllGarbage(i::Heap::kNoGCFlags);
  for (int i = 0; i < kElementCount; i++) {
    CHECK_EQ(static_cast<int64_t>(static_cast<ElementType>(i)),
             static_cast<int64_t>(fixed_array->get_scalar(i)));
  }
  v8::Handle<v8::Object> obj = v8::Object::New(CcTest::isolate());
  i::Handle<i::JSObject> jsobj = v8::Utils::OpenHandle(*obj);
  i::Handle<i::Map> fixed_array_map =
      i::JSObject::GetElementsTransitionMap(jsobj, elements_kind);
  jsobj->set_map(*fixed_array_map);
  jsobj->set_elements(*fixed_array);

  ObjectWithExternalArrayTestHelper<FixedTypedArrayClass, ElementType>(
      context.local(), obj, kElementCount, array_type,
      static_cast<int64_t>(low),
      static_cast<int64_t>(high));
}


THREADED_TEST(FixedUint8Array) {
  FixedTypedArrayTestHelper<i::FixedUint8Array, i::UINT8_ELEMENTS, uint8_t>(
    v8::kExternalUint8Array,
    0x0, 0xFF);
}


THREADED_TEST(FixedUint8ClampedArray) {
  FixedTypedArrayTestHelper<i::FixedUint8ClampedArray,
                            i::UINT8_CLAMPED_ELEMENTS, uint8_t>(
    v8::kExternalUint8ClampedArray,
    0x0, 0xFF);
}


THREADED_TEST(FixedInt8Array) {
  FixedTypedArrayTestHelper<i::FixedInt8Array, i::INT8_ELEMENTS, int8_t>(
    v8::kExternalInt8Array,
    -0x80, 0x7F);
}


THREADED_TEST(FixedUint16Array) {
  FixedTypedArrayTestHelper<i::FixedUint16Array, i::UINT16_ELEMENTS, uint16_t>(
    v8::kExternalUint16Array,
    0x0, 0xFFFF);
}


THREADED_TEST(FixedInt16Array) {
  FixedTypedArrayTestHelper<i::FixedInt16Array, i::INT16_ELEMENTS, int16_t>(
    v8::kExternalInt16Array,
    -0x8000, 0x7FFF);
}


THREADED_TEST(FixedUint32Array) {
  FixedTypedArrayTestHelper<i::FixedUint32Array, i::UINT32_ELEMENTS, uint32_t>(
    v8::kExternalUint32Array,
    0x0, UINT_MAX);
}


THREADED_TEST(FixedInt32Array) {
  FixedTypedArrayTestHelper<i::FixedInt32Array, i::INT32_ELEMENTS, int32_t>(
    v8::kExternalInt32Array,
    INT_MIN, INT_MAX);
}


THREADED_TEST(FixedFloat32Array) {
  FixedTypedArrayTestHelper<i::FixedFloat32Array, i::FLOAT32_ELEMENTS, float>(
    v8::kExternalFloat32Array,
    -500, 500);
}


THREADED_TEST(FixedFloat64Array) {
  FixedTypedArrayTestHelper<i::FixedFloat64Array, i::FLOAT64_ELEMENTS, float>(
    v8::kExternalFloat64Array,
    -500, 500);
}


template <class ExternalArrayClass, class ElementType>
static void ExternalArrayTestHelper(v8::ExternalArrayType array_type,
                                    int64_t low,
                                    int64_t high) {
  LocalContext context;
  i::Isolate* isolate = CcTest::i_isolate();
  i::Factory* factory = isolate->factory();
  v8::HandleScope scope(context->GetIsolate());
  const int kElementCount = 40;
  int element_size = ExternalArrayElementSize(array_type);
  ElementType* array_data =
      static_cast<ElementType*>(malloc(kElementCount * element_size));
  i::Handle<ExternalArrayClass> array =
      i::Handle<ExternalArrayClass>::cast(
          factory->NewExternalArray(kElementCount, array_type, array_data));
  // Force GC to trigger verification.
  CcTest::heap()->CollectAllGarbage(i::Heap::kNoGCFlags);
  for (int i = 0; i < kElementCount; i++) {
    array->set(i, static_cast<ElementType>(i));
  }
  // Force GC to trigger verification.
  CcTest::heap()->CollectAllGarbage(i::Heap::kNoGCFlags);
  for (int i = 0; i < kElementCount; i++) {
    CHECK_EQ(static_cast<int64_t>(i),
             static_cast<int64_t>(array->get_scalar(i)));
    CHECK_EQ(static_cast<int64_t>(i), static_cast<int64_t>(array_data[i]));
  }

  v8::Handle<v8::Object> obj = v8::Object::New(context->GetIsolate());
  i::Handle<i::JSObject> jsobj = v8::Utils::OpenHandle(*obj);
  // Set the elements to be the external array.
  obj->SetIndexedPropertiesToExternalArrayData(array_data,
                                               array_type,
                                               kElementCount);
  CHECK_EQ(1,
           static_cast<int>(
               i::Object::GetElement(
                   isolate, jsobj, 1).ToHandleChecked()->Number()));

  ObjectWithExternalArrayTestHelper<ExternalArrayClass, ElementType>(
      context.local(), obj, kElementCount, array_type, low, high);

  v8::Handle<v8::Value> result;

  // Test more complex manipulations which cause eax to contain values
  // that won't be completely overwritten by loads from the arrays.
  // This catches bugs in the instructions used for the KeyedLoadIC
  // for byte and word types.
  {
    const int kXSize = 300;
    const int kYSize = 300;
    const int kLargeElementCount = kXSize * kYSize * 4;
    ElementType* large_array_data =
        static_cast<ElementType*>(malloc(kLargeElementCount * element_size));
    v8::Handle<v8::Object> large_obj = v8::Object::New(context->GetIsolate());
    // Set the elements to be the external array.
    large_obj->SetIndexedPropertiesToExternalArrayData(large_array_data,
                                                       array_type,
                                                       kLargeElementCount);
    context->Global()->Set(v8_str("large_array"), large_obj);
    // Initialize contents of a few rows.
    for (int x = 0; x < 300; x++) {
      int row = 0;
      int offset = row * 300 * 4;
      large_array_data[offset + 4 * x + 0] = (ElementType) 127;
      large_array_data[offset + 4 * x + 1] = (ElementType) 0;
      large_array_data[offset + 4 * x + 2] = (ElementType) 0;
      large_array_data[offset + 4 * x + 3] = (ElementType) 127;
      row = 150;
      offset = row * 300 * 4;
      large_array_data[offset + 4 * x + 0] = (ElementType) 127;
      large_array_data[offset + 4 * x + 1] = (ElementType) 0;
      large_array_data[offset + 4 * x + 2] = (ElementType) 0;
      large_array_data[offset + 4 * x + 3] = (ElementType) 127;
      row = 298;
      offset = row * 300 * 4;
      large_array_data[offset + 4 * x + 0] = (ElementType) 127;
      large_array_data[offset + 4 * x + 1] = (ElementType) 0;
      large_array_data[offset + 4 * x + 2] = (ElementType) 0;
      large_array_data[offset + 4 * x + 3] = (ElementType) 127;
    }
    // The goal of the code below is to make "offset" large enough
    // that the computation of the index (which goes into eax) has
    // high bits set which will not be overwritten by a byte or short
    // load.
    result = CompileRun("var failed = false;"
                        "var offset = 0;"
                        "for (var i = 0; i < 300; i++) {"
                        "  if (large_array[4 * i] != 127 ||"
                        "      large_array[4 * i + 1] != 0 ||"
                        "      large_array[4 * i + 2] != 0 ||"
                        "      large_array[4 * i + 3] != 127) {"
                        "    failed = true;"
                        "  }"
                        "}"
                        "offset = 150 * 300 * 4;"
                        "for (var i = 0; i < 300; i++) {"
                        "  if (large_array[offset + 4 * i] != 127 ||"
                        "      large_array[offset + 4 * i + 1] != 0 ||"
                        "      large_array[offset + 4 * i + 2] != 0 ||"
                        "      large_array[offset + 4 * i + 3] != 127) {"
                        "    failed = true;"
                        "  }"
                        "}"
                        "offset = 298 * 300 * 4;"
                        "for (var i = 0; i < 300; i++) {"
                        "  if (large_array[offset + 4 * i] != 127 ||"
                        "      large_array[offset + 4 * i + 1] != 0 ||"
                        "      large_array[offset + 4 * i + 2] != 0 ||"
                        "      large_array[offset + 4 * i + 3] != 127) {"
                        "    failed = true;"
                        "  }"
                        "}"
                        "!failed;");
    CHECK_EQ(true, result->BooleanValue());
    free(large_array_data);
  }

  // The "" property descriptor is overloaded to store information about
  // the external array. Ensure that setting and accessing the "" property
  // works (it should overwrite the information cached about the external
  // array in the DescriptorArray) in various situations.
  result = CompileRun("ext_array[''] = 23; ext_array['']");
  CHECK_EQ(23, result->Int32Value());

  // Property "" set after the external array is associated with the object.
  {
    v8::Handle<v8::Object> obj2 = v8::Object::New(context->GetIsolate());
    obj2->Set(v8_str("ee_test_field"),
              v8::Int32::New(context->GetIsolate(), 256));
    obj2->Set(v8_str(""), v8::Int32::New(context->GetIsolate(), 1503));
    // Set the elements to be the external array.
    obj2->SetIndexedPropertiesToExternalArrayData(array_data,
                                                  array_type,
                                                  kElementCount);
    context->Global()->Set(v8_str("ext_array"), obj2);
    result = CompileRun("ext_array['']");
    CHECK_EQ(1503, result->Int32Value());
  }

  // Property "" set after the external array is associated with the object.
  {
    v8::Handle<v8::Object> obj2 = v8::Object::New(context->GetIsolate());
    obj2->Set(v8_str("ee_test_field_2"),
              v8::Int32::New(context->GetIsolate(), 256));
    // Set the elements to be the external array.
    obj2->SetIndexedPropertiesToExternalArrayData(array_data,
                                                  array_type,
                                                  kElementCount);
    obj2->Set(v8_str(""), v8::Int32::New(context->GetIsolate(), 1503));
    context->Global()->Set(v8_str("ext_array"), obj2);
    result = CompileRun("ext_array['']");
    CHECK_EQ(1503, result->Int32Value());
  }

  // Should reuse the map from previous test.
  {
    v8::Handle<v8::Object> obj2 = v8::Object::New(context->GetIsolate());
    obj2->Set(v8_str("ee_test_field_2"),
              v8::Int32::New(context->GetIsolate(), 256));
    // Set the elements to be the external array. Should re-use the map
    // from previous test.
    obj2->SetIndexedPropertiesToExternalArrayData(array_data,
                                                  array_type,
                                                  kElementCount);
    context->Global()->Set(v8_str("ext_array"), obj2);
    result = CompileRun("ext_array['']");
  }

  // Property "" is a constant function that shouldn't not be interfered with
  // when an external array is set.
  {
    v8::Handle<v8::Object> obj2 = v8::Object::New(context->GetIsolate());
    // Start
    obj2->Set(v8_str("ee_test_field3"),
              v8::Int32::New(context->GetIsolate(), 256));

    // Add a constant function to an object.
    context->Global()->Set(v8_str("ext_array"), obj2);
    result = CompileRun("ext_array[''] = function() {return 1503;};"
                        "ext_array['']();");

    // Add an external array transition to the same map that
    // has the constant transition.
    v8::Handle<v8::Object> obj3 = v8::Object::New(context->GetIsolate());
    obj3->Set(v8_str("ee_test_field3"),
              v8::Int32::New(context->GetIsolate(), 256));
    obj3->SetIndexedPropertiesToExternalArrayData(array_data,
                                                  array_type,
                                                  kElementCount);
    context->Global()->Set(v8_str("ext_array"), obj3);
  }

  // If a external array transition is in the map, it should get clobbered
  // by a constant function.
  {
    // Add an external array transition.
    v8::Handle<v8::Object> obj3 = v8::Object::New(context->GetIsolate());
    obj3->Set(v8_str("ee_test_field4"),
              v8::Int32::New(context->GetIsolate(), 256));
    obj3->SetIndexedPropertiesToExternalArrayData(array_data,
                                                  array_type,
                                                  kElementCount);

    // Add a constant function to the same map that just got an external array
    // transition.
    v8::Handle<v8::Object> obj2 = v8::Object::New(context->GetIsolate());
    obj2->Set(v8_str("ee_test_field4"),
              v8::Int32::New(context->GetIsolate(), 256));
    context->Global()->Set(v8_str("ext_array"), obj2);
    result = CompileRun("ext_array[''] = function() {return 1503;};"
                        "ext_array['']();");
  }

  free(array_data);
}


THREADED_TEST(ExternalInt8Array) {
  ExternalArrayTestHelper<i::ExternalInt8Array, int8_t>(
      v8::kExternalInt8Array,
      -128,
      127);
}


THREADED_TEST(ExternalUint8Array) {
  ExternalArrayTestHelper<i::ExternalUint8Array, uint8_t>(
      v8::kExternalUint8Array,
      0,
      255);
}


THREADED_TEST(ExternalUint8ClampedArray) {
  ExternalArrayTestHelper<i::ExternalUint8ClampedArray, uint8_t>(
      v8::kExternalUint8ClampedArray,
      0,
      255);
}


THREADED_TEST(ExternalInt16Array) {
  ExternalArrayTestHelper<i::ExternalInt16Array, int16_t>(
      v8::kExternalInt16Array,
      -32768,
      32767);
}


THREADED_TEST(ExternalUint16Array) {
  ExternalArrayTestHelper<i::ExternalUint16Array, uint16_t>(
      v8::kExternalUint16Array,
      0,
      65535);
}


THREADED_TEST(ExternalInt32Array) {
  ExternalArrayTestHelper<i::ExternalInt32Array, int32_t>(
      v8::kExternalInt32Array,
      INT_MIN,   // -2147483648
      INT_MAX);  //  2147483647
}


THREADED_TEST(ExternalUint32Array) {
  ExternalArrayTestHelper<i::ExternalUint32Array, uint32_t>(
      v8::kExternalUint32Array,
      0,
      UINT_MAX);  // 4294967295
}


THREADED_TEST(ExternalFloat32Array) {
  ExternalArrayTestHelper<i::ExternalFloat32Array, float>(
      v8::kExternalFloat32Array,
      -500,
      500);
}


THREADED_TEST(ExternalFloat64Array) {
  ExternalArrayTestHelper<i::ExternalFloat64Array, double>(
      v8::kExternalFloat64Array,
      -500,
      500);
}


THREADED_TEST(ExternalArrays) {
  TestExternalInt8Array();
  TestExternalUint8Array();
  TestExternalInt16Array();
  TestExternalUint16Array();
  TestExternalInt32Array();
  TestExternalUint32Array();
  TestExternalFloat32Array();
}


void ExternalArrayInfoTestHelper(v8::ExternalArrayType array_type) {
  LocalContext context;
  v8::HandleScope scope(context->GetIsolate());
  for (int size = 0; size < 100; size += 10) {
    int element_size = ExternalArrayElementSize(array_type);
    void* external_data = malloc(size * element_size);
    v8::Handle<v8::Object> obj = v8::Object::New(context->GetIsolate());
    obj->SetIndexedPropertiesToExternalArrayData(
        external_data, array_type, size);
    CHECK(obj->HasIndexedPropertiesInExternalArrayData());
    CHECK_EQ(external_data, obj->GetIndexedPropertiesExternalArrayData());
    CHECK_EQ(array_type, obj->GetIndexedPropertiesExternalArrayDataType());
    CHECK_EQ(size, obj->GetIndexedPropertiesExternalArrayDataLength());
    free(external_data);
  }
}


THREADED_TEST(ExternalArrayInfo) {
  ExternalArrayInfoTestHelper(v8::kExternalInt8Array);
  ExternalArrayInfoTestHelper(v8::kExternalUint8Array);
  ExternalArrayInfoTestHelper(v8::kExternalInt16Array);
  ExternalArrayInfoTestHelper(v8::kExternalUint16Array);
  ExternalArrayInfoTestHelper(v8::kExternalInt32Array);
  ExternalArrayInfoTestHelper(v8::kExternalUint32Array);
  ExternalArrayInfoTestHelper(v8::kExternalFloat32Array);
  ExternalArrayInfoTestHelper(v8::kExternalFloat64Array);
  ExternalArrayInfoTestHelper(v8::kExternalUint8ClampedArray);
}


void ExtArrayLimitsHelper(v8::Isolate* isolate,
                          v8::ExternalArrayType array_type,
                          int size) {
  v8::Handle<v8::Object> obj = v8::Object::New(isolate);
  v8::V8::SetFatalErrorHandler(StoringErrorCallback);
  last_location = last_message = NULL;
  obj->SetIndexedPropertiesToExternalArrayData(NULL, array_type, size);
  CHECK(!obj->HasIndexedPropertiesInExternalArrayData());
  CHECK(last_location);
  CHECK(last_message);
}


TEST(ExternalArrayLimits) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);
  ExtArrayLimitsHelper(isolate, v8::kExternalInt8Array, 0x40000000);
  ExtArrayLimitsHelper(isolate, v8::kExternalInt8Array, 0xffffffff);
  ExtArrayLimitsHelper(isolate, v8::kExternalUint8Array, 0x40000000);
  ExtArrayLimitsHelper(isolate, v8::kExternalUint8Array, 0xffffffff);
  ExtArrayLimitsHelper(isolate, v8::kExternalInt16Array, 0x40000000);
  ExtArrayLimitsHelper(isolate, v8::kExternalInt16Array, 0xffffffff);
  ExtArrayLimitsHelper(isolate, v8::kExternalUint16Array, 0x40000000);
  ExtArrayLimitsHelper(isolate, v8::kExternalUint16Array, 0xffffffff);
  ExtArrayLimitsHelper(isolate, v8::kExternalInt32Array, 0x40000000);
  ExtArrayLimitsHelper(isolate, v8::kExternalInt32Array, 0xffffffff);
  ExtArrayLimitsHelper(isolate, v8::kExternalUint32Array, 0x40000000);
  ExtArrayLimitsHelper(isolate, v8::kExternalUint32Array, 0xffffffff);
  ExtArrayLimitsHelper(isolate, v8::kExternalFloat32Array, 0x40000000);
  ExtArrayLimitsHelper(isolate, v8::kExternalFloat32Array, 0xffffffff);
  ExtArrayLimitsHelper(isolate, v8::kExternalFloat64Array, 0x40000000);
  ExtArrayLimitsHelper(isolate, v8::kExternalFloat64Array, 0xffffffff);
  ExtArrayLimitsHelper(isolate, v8::kExternalUint8ClampedArray, 0x40000000);
  ExtArrayLimitsHelper(isolate, v8::kExternalUint8ClampedArray, 0xffffffff);
}


template <typename ElementType, typename TypedArray,
          class ExternalArrayClass>
void TypedArrayTestHelper(v8::ExternalArrayType array_type,
                          int64_t low, int64_t high) {
  const int kElementCount = 50;

  i::ScopedVector<ElementType> backing_store(kElementCount+2);

  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope handle_scope(isolate);

  Local<v8::ArrayBuffer> ab =
      v8::ArrayBuffer::New(isolate, backing_store.start(),
                           (kElementCount + 2) * sizeof(ElementType));
  Local<TypedArray> ta =
      TypedArray::New(ab, 2*sizeof(ElementType), kElementCount);
  CheckInternalFieldsAreZero<v8::ArrayBufferView>(ta);
  CHECK_EQ(kElementCount, static_cast<int>(ta->Length()));
  CHECK_EQ(2 * sizeof(ElementType), ta->ByteOffset());
  CHECK_EQ(kElementCount * sizeof(ElementType), ta->ByteLength());
  CHECK(ab->Equals(ta->Buffer()));

  ElementType* data = backing_store.start() + 2;
  for (int i = 0; i < kElementCount; i++) {
    data[i] = static_cast<ElementType>(i);
  }

  ObjectWithExternalArrayTestHelper<ExternalArrayClass, ElementType>(
      env.local(), ta, kElementCount, array_type, low, high);
}


THREADED_TEST(Uint8Array) {
  TypedArrayTestHelper<uint8_t, v8::Uint8Array, i::ExternalUint8Array>(
      v8::kExternalUint8Array, 0, 0xFF);
}


THREADED_TEST(Int8Array) {
  TypedArrayTestHelper<int8_t, v8::Int8Array, i::ExternalInt8Array>(
      v8::kExternalInt8Array, -0x80, 0x7F);
}


THREADED_TEST(Uint16Array) {
  TypedArrayTestHelper<uint16_t,
                       v8::Uint16Array,
                       i::ExternalUint16Array>(
      v8::kExternalUint16Array, 0, 0xFFFF);
}


THREADED_TEST(Int16Array) {
  TypedArrayTestHelper<int16_t, v8::Int16Array, i::ExternalInt16Array>(
      v8::kExternalInt16Array, -0x8000, 0x7FFF);
}


THREADED_TEST(Uint32Array) {
  TypedArrayTestHelper<uint32_t, v8::Uint32Array, i::ExternalUint32Array>(
      v8::kExternalUint32Array, 0, UINT_MAX);
}


THREADED_TEST(Int32Array) {
  TypedArrayTestHelper<int32_t, v8::Int32Array, i::ExternalInt32Array>(
      v8::kExternalInt32Array, INT_MIN, INT_MAX);
}


THREADED_TEST(Float32Array) {
  TypedArrayTestHelper<float, v8::Float32Array, i::ExternalFloat32Array>(
      v8::kExternalFloat32Array, -500, 500);
}


THREADED_TEST(Float64Array) {
  TypedArrayTestHelper<double, v8::Float64Array, i::ExternalFloat64Array>(
      v8::kExternalFloat64Array, -500, 500);
}


THREADED_TEST(Uint8ClampedArray) {
  TypedArrayTestHelper<uint8_t,
                       v8::Uint8ClampedArray, i::ExternalUint8ClampedArray>(
      v8::kExternalUint8ClampedArray, 0, 0xFF);
}


THREADED_TEST(DataView) {
  const int kSize = 50;

  i::ScopedVector<uint8_t> backing_store(kSize+2);

  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope handle_scope(isolate);

  Local<v8::ArrayBuffer> ab =
      v8::ArrayBuffer::New(isolate, backing_store.start(), 2 + kSize);
  Local<v8::DataView> dv =
      v8::DataView::New(ab, 2, kSize);
  CheckInternalFieldsAreZero<v8::ArrayBufferView>(dv);
  CHECK_EQ(2u, dv->ByteOffset());
  CHECK_EQ(kSize, static_cast<int>(dv->ByteLength()));
  CHECK(ab->Equals(dv->Buffer()));
}


#define IS_ARRAY_BUFFER_VIEW_TEST(View)                                       \
  THREADED_TEST(Is##View) {                                                   \
    LocalContext env;                                                         \
    v8::Isolate* isolate = env->GetIsolate();                                 \
    v8::HandleScope handle_scope(isolate);                                    \
                                                                              \
    Handle<Value> result = CompileRun(                                        \
        "var ab = new ArrayBuffer(128);"                                      \
        "new " #View "(ab)");                                                 \
    CHECK(result->IsArrayBufferView());                                       \
    CHECK(result->Is##View());                                                \
    CheckInternalFieldsAreZero<v8::ArrayBufferView>(result.As<v8::View>());   \
  }

IS_ARRAY_BUFFER_VIEW_TEST(Uint8Array)
IS_ARRAY_BUFFER_VIEW_TEST(Int8Array)
IS_ARRAY_BUFFER_VIEW_TEST(Uint16Array)
IS_ARRAY_BUFFER_VIEW_TEST(Int16Array)
IS_ARRAY_BUFFER_VIEW_TEST(Uint32Array)
IS_ARRAY_BUFFER_VIEW_TEST(Int32Array)
IS_ARRAY_BUFFER_VIEW_TEST(Float32Array)
IS_ARRAY_BUFFER_VIEW_TEST(Float64Array)
IS_ARRAY_BUFFER_VIEW_TEST(Uint8ClampedArray)
IS_ARRAY_BUFFER_VIEW_TEST(DataView)

#undef IS_ARRAY_BUFFER_VIEW_TEST



THREADED_TEST(ScriptContextDependence) {
  LocalContext c1;
  v8::HandleScope scope(c1->GetIsolate());
  const char *source = "foo";
  v8::Handle<v8::Script> dep = v8_compile(source);
  v8::ScriptCompiler::Source script_source(v8::String::NewFromUtf8(
      c1->GetIsolate(), source));
  v8::Handle<v8::UnboundScript> indep =
      v8::ScriptCompiler::CompileUnbound(c1->GetIsolate(), &script_source);
  c1->Global()->Set(v8::String::NewFromUtf8(c1->GetIsolate(), "foo"),
                    v8::Integer::New(c1->GetIsolate(), 100));
  CHECK_EQ(dep->Run()->Int32Value(), 100);
  CHECK_EQ(indep->BindToCurrentContext()->Run()->Int32Value(), 100);
  LocalContext c2;
  c2->Global()->Set(v8::String::NewFromUtf8(c2->GetIsolate(), "foo"),
                    v8::Integer::New(c2->GetIsolate(), 101));
  CHECK_EQ(dep->Run()->Int32Value(), 100);
  CHECK_EQ(indep->BindToCurrentContext()->Run()->Int32Value(), 101);
}


THREADED_TEST(StackTrace) {
  LocalContext context;
  v8::HandleScope scope(context->GetIsolate());
  v8::TryCatch try_catch;
  const char *source = "function foo() { FAIL.FAIL; }; foo();";
  v8::Handle<v8::String> src =
      v8::String::NewFromUtf8(context->GetIsolate(), source);
  v8::Handle<v8::String> origin =
      v8::String::NewFromUtf8(context->GetIsolate(), "stack-trace-test");
  v8::ScriptCompiler::Source script_source(src, v8::ScriptOrigin(origin));
  v8::ScriptCompiler::CompileUnbound(context->GetIsolate(), &script_source)
      ->BindToCurrentContext()
      ->Run();
  CHECK(try_catch.HasCaught());
  v8::String::Utf8Value stack(try_catch.StackTrace());
  CHECK(strstr(*stack, "at foo (stack-trace-test") != NULL);
}


// Checks that a StackFrame has certain expected values.
void checkStackFrame(const char* expected_script_name,
    const char* expected_func_name, int expected_line_number,
    int expected_column, bool is_eval, bool is_constructor,
    v8::Handle<v8::StackFrame> frame) {
  v8::HandleScope scope(CcTest::isolate());
  v8::String::Utf8Value func_name(frame->GetFunctionName());
  v8::String::Utf8Value script_name(frame->GetScriptName());
  if (*script_name == NULL) {
    // The situation where there is no associated script, like for evals.
    CHECK(expected_script_name == NULL);
  } else {
    CHECK(strstr(*script_name, expected_script_name) != NULL);
  }
  CHECK(strstr(*func_name, expected_func_name) != NULL);
  CHECK_EQ(expected_line_number, frame->GetLineNumber());
  CHECK_EQ(expected_column, frame->GetColumn());
  CHECK_EQ(is_eval, frame->IsEval());
  CHECK_EQ(is_constructor, frame->IsConstructor());
}


void AnalyzeStackInNativeCode(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::HandleScope scope(args.GetIsolate());
  const char* origin = "capture-stack-trace-test";
  const int kOverviewTest = 1;
  const int kDetailedTest = 2;

  DCHECK(args.Length() == 1);

  int testGroup = args[0]->Int32Value();
  if (testGroup == kOverviewTest) {
    v8::Handle<v8::StackTrace> stackTrace = v8::StackTrace::CurrentStackTrace(
        args.GetIsolate(), 10, v8::StackTrace::kOverview);
    CHECK_EQ(4, stackTrace->GetFrameCount());
    checkStackFrame(origin, "bar", 2, 10, false, false,
                    stackTrace->GetFrame(0));
    checkStackFrame(origin, "foo", 6, 3, false, false,
                    stackTrace->GetFrame(1));
    // This is the source string inside the eval which has the call to foo.
    checkStackFrame(NULL, "", 1, 5, false, false,
                    stackTrace->GetFrame(2));
    // The last frame is an anonymous function which has the initial eval call.
    checkStackFrame(origin, "", 8, 7, false, false,
                    stackTrace->GetFrame(3));

    CHECK(stackTrace->AsArray()->IsArray());
  } else if (testGroup == kDetailedTest) {
    v8::Handle<v8::StackTrace> stackTrace = v8::StackTrace::CurrentStackTrace(
        args.GetIsolate(), 10, v8::StackTrace::kDetailed);
    CHECK_EQ(4, stackTrace->GetFrameCount());
    checkStackFrame(origin, "bat", 4, 22, false, false,
                    stackTrace->GetFrame(0));
    checkStackFrame(origin, "baz", 8, 3, false, true,
                    stackTrace->GetFrame(1));
    bool is_eval = true;
    // This is the source string inside the eval which has the call to baz.
    checkStackFrame(NULL, "", 1, 5, is_eval, false,
                    stackTrace->GetFrame(2));
    // The last frame is an anonymous function which has the initial eval call.
    checkStackFrame(origin, "", 10, 1, false, false,
                    stackTrace->GetFrame(3));

    CHECK(stackTrace->AsArray()->IsArray());
  }
}


// Tests the C++ StackTrace API.
// TODO(3074796): Reenable this as a THREADED_TEST once it passes.
// THREADED_TEST(CaptureStackTrace) {
TEST(CaptureStackTrace) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  v8::Handle<v8::String> origin =
      v8::String::NewFromUtf8(isolate, "capture-stack-trace-test");
  Local<ObjectTemplate> templ = ObjectTemplate::New(isolate);
  templ->Set(v8_str("AnalyzeStackInNativeCode"),
             v8::FunctionTemplate::New(isolate, AnalyzeStackInNativeCode));
  LocalContext context(0, templ);

  // Test getting OVERVIEW information. Should ignore information that is not
  // script name, function name, line number, and column offset.
  const char *overview_source =
    "function bar() {\n"
    "  var y; AnalyzeStackInNativeCode(1);\n"
    "}\n"
    "function foo() {\n"
    "\n"
    "  bar();\n"
    "}\n"
    "var x;eval('new foo();');";
  v8::Handle<v8::String> overview_src =
      v8::String::NewFromUtf8(isolate, overview_source);
  v8::ScriptCompiler::Source script_source(overview_src,
                                           v8::ScriptOrigin(origin));
  v8::Handle<Value> overview_result(
      v8::ScriptCompiler::CompileUnbound(isolate, &script_source)
          ->BindToCurrentContext()
          ->Run());
  CHECK(!overview_result.IsEmpty());
  CHECK(overview_result->IsObject());

  // Test getting DETAILED information.
  const char *detailed_source =
    "function bat() {AnalyzeStackInNativeCode(2);\n"
    "}\n"
    "\n"
    "function baz() {\n"
    "  bat();\n"
    "}\n"
    "eval('new baz();');";
  v8::Handle<v8::String> detailed_src =
      v8::String::NewFromUtf8(isolate, detailed_source);
  // Make the script using a non-zero line and column offset.
  v8::Handle<v8::Integer> line_offset = v8::Integer::New(isolate, 3);
  v8::Handle<v8::Integer> column_offset = v8::Integer::New(isolate, 5);
  v8::ScriptOrigin detailed_origin(origin, line_offset, column_offset);
  v8::ScriptCompiler::Source script_source2(detailed_src, detailed_origin);
  v8::Handle<v8::UnboundScript> detailed_script(
      v8::ScriptCompiler::CompileUnbound(isolate, &script_source2));
  v8::Handle<Value> detailed_result(
      detailed_script->BindToCurrentContext()->Run());
  CHECK(!detailed_result.IsEmpty());
  CHECK(detailed_result->IsObject());
}


static void StackTraceForUncaughtExceptionListener(
    v8::Handle<v8::Message> message,
    v8::Handle<Value>) {
  report_count++;
  v8::Handle<v8::StackTrace> stack_trace = message->GetStackTrace();
  CHECK_EQ(2, stack_trace->GetFrameCount());
  checkStackFrame("origin", "foo", 2, 3, false, false,
                  stack_trace->GetFrame(0));
  checkStackFrame("origin", "bar", 5, 3, false, false,
                  stack_trace->GetFrame(1));
}


TEST(CaptureStackTraceForUncaughtException) {
  report_count = 0;
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());
  v8::V8::AddMessageListener(StackTraceForUncaughtExceptionListener);
  v8::V8::SetCaptureStackTraceForUncaughtExceptions(true);

  CompileRunWithOrigin(
      "function foo() {\n"
      "  throw 1;\n"
      "};\n"
      "function bar() {\n"
      "  foo();\n"
      "};",
      "origin");
  v8::Local<v8::Object> global = env->Global();
  Local<Value> trouble = global->Get(v8_str("bar"));
  CHECK(trouble->IsFunction());
  Function::Cast(*trouble)->Call(global, 0, NULL);
  v8::V8::SetCaptureStackTraceForUncaughtExceptions(false);
  v8::V8::RemoveMessageListeners(StackTraceForUncaughtExceptionListener);
  CHECK_EQ(1, report_count);
}


TEST(GetStackTraceForUncaughtExceptionFromSimpleStackTrace) {
  report_count = 0;
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());

  // Create an Error object first.
  CompileRunWithOrigin(
      "function foo() {\n"
      "e=new Error('err');\n"
      "};\n"
      "function bar() {\n"
      "  foo();\n"
      "};\n"
      "var e;",
      "origin");
  v8::Local<v8::Object> global = env->Global();
  Local<Value> trouble = global->Get(v8_str("bar"));
  CHECK(trouble->IsFunction());
  Function::Cast(*trouble)->Call(global, 0, NULL);

  // Enable capturing detailed stack trace late, and throw the exception.
  // The detailed stack trace should be extracted from the simple stack.
  v8::V8::AddMessageListener(StackTraceForUncaughtExceptionListener);
  v8::V8::SetCaptureStackTraceForUncaughtExceptions(true);
  CompileRunWithOrigin("throw e", "origin");
  v8::V8::SetCaptureStackTraceForUncaughtExceptions(false);
  v8::V8::RemoveMessageListeners(StackTraceForUncaughtExceptionListener);
  CHECK_EQ(1, report_count);
}


TEST(CaptureStackTraceForUncaughtExceptionAndSetters) {
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());
  v8::V8::SetCaptureStackTraceForUncaughtExceptions(true,
                                                    1024,
                                                    v8::StackTrace::kDetailed);

  CompileRun(
      "var setters = ['column', 'lineNumber', 'scriptName',\n"
      "    'scriptNameOrSourceURL', 'functionName', 'isEval',\n"
      "    'isConstructor'];\n"
      "for (var i = 0; i < setters.length; i++) {\n"
      "  var prop = setters[i];\n"
      "  Object.prototype.__defineSetter__(prop, function() { throw prop; });\n"
      "}\n");
  CompileRun("throw 'exception';");
  v8::V8::SetCaptureStackTraceForUncaughtExceptions(false);
}


static void StackTraceFunctionNameListener(v8::Handle<v8::Message> message,
                                           v8::Handle<Value>) {
  v8::Handle<v8::StackTrace> stack_trace = message->GetStackTrace();
  CHECK_EQ(5, stack_trace->GetFrameCount());
  checkStackFrame("origin", "foo:0", 4, 7, false, false,
                  stack_trace->GetFrame(0));
  checkStackFrame("origin", "foo:1", 5, 27, false, false,
                  stack_trace->GetFrame(1));
  checkStackFrame("origin", "foo", 5, 27, false, false,
                  stack_trace->GetFrame(2));
  checkStackFrame("origin", "foo", 5, 27, false, false,
                  stack_trace->GetFrame(3));
  checkStackFrame("origin", "", 1, 14, false, false, stack_trace->GetFrame(4));
}


TEST(GetStackTraceContainsFunctionsWithFunctionName) {
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());

  CompileRunWithOrigin(
      "function gen(name, counter) {\n"
      "  var f = function foo() {\n"
      "    if (counter === 0)\n"
      "      throw 1;\n"
      "    gen(name, counter - 1)();\n"
      "  };\n"
      "  if (counter == 3) {\n"
      "    Object.defineProperty(f, 'name', {get: function(){ throw 239; }});\n"
      "  } else {\n"
      "    Object.defineProperty(f, 'name', {writable:true});\n"
      "    if (counter == 2)\n"
      "      f.name = 42;\n"
      "    else\n"
      "      f.name = name + ':' + counter;\n"
      "  }\n"
      "  return f;\n"
      "};",
      "origin");

  v8::V8::AddMessageListener(StackTraceFunctionNameListener);
  v8::V8::SetCaptureStackTraceForUncaughtExceptions(true);
  CompileRunWithOrigin("gen('foo', 3)();", "origin");
  v8::V8::SetCaptureStackTraceForUncaughtExceptions(false);
  v8::V8::RemoveMessageListeners(StackTraceFunctionNameListener);
}


static void RethrowStackTraceHandler(v8::Handle<v8::Message> message,
                                     v8::Handle<v8::Value> data) {
  // Use the frame where JavaScript is called from.
  v8::Handle<v8::StackTrace> stack_trace = message->GetStackTrace();
  CHECK(!stack_trace.IsEmpty());
  int frame_count = stack_trace->GetFrameCount();
  CHECK_EQ(3, frame_count);
  int line_number[] = {1, 2, 5};
  for (int i = 0; i < frame_count; i++) {
    CHECK_EQ(line_number[i], stack_trace->GetFrame(i)->GetLineNumber());
  }
}


// Test that we only return the stack trace at the site where the exception
// is first thrown (not where it is rethrown).
TEST(RethrowStackTrace) {
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());
  // We make sure that
  // - the stack trace of the ReferenceError in g() is reported.
  // - the stack trace is not overwritten when e1 is rethrown by t().
  // - the stack trace of e2 does not overwrite that of e1.
  const char* source =
      "function g() { error; }          \n"
      "function f() { g(); }            \n"
      "function t(e) { throw e; }       \n"
      "try {                            \n"
      "  f();                           \n"
      "} catch (e1) {                   \n"
      "  try {                          \n"
      "    error;                       \n"
      "  } catch (e2) {                 \n"
      "    t(e1);                       \n"
      "  }                              \n"
      "}                                \n";
  v8::V8::AddMessageListener(RethrowStackTraceHandler);
  v8::V8::SetCaptureStackTraceForUncaughtExceptions(true);
  CompileRun(source);
  v8::V8::SetCaptureStackTraceForUncaughtExceptions(false);
  v8::V8::RemoveMessageListeners(RethrowStackTraceHandler);
}


static void RethrowPrimitiveStackTraceHandler(v8::Handle<v8::Message> message,
                                              v8::Handle<v8::Value> data) {
  v8::Handle<v8::StackTrace> stack_trace = message->GetStackTrace();
  CHECK(!stack_trace.IsEmpty());
  int frame_count = stack_trace->GetFrameCount();
  CHECK_EQ(2, frame_count);
  int line_number[] = {3, 7};
  for (int i = 0; i < frame_count; i++) {
    CHECK_EQ(line_number[i], stack_trace->GetFrame(i)->GetLineNumber());
  }
}


// Test that we do not recognize identity for primitive exceptions.
TEST(RethrowPrimitiveStackTrace) {
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());
  // We do not capture stack trace for non Error objects on creation time.
  // Instead, we capture the stack trace on last throw.
  const char* source =
      "function g() { throw 404; }      \n"
      "function f() { g(); }            \n"
      "function t(e) { throw e; }       \n"
      "try {                            \n"
      "  f();                           \n"
      "} catch (e1) {                   \n"
      "  t(e1)                          \n"
      "}                                \n";
  v8::V8::AddMessageListener(RethrowPrimitiveStackTraceHandler);
  v8::V8::SetCaptureStackTraceForUncaughtExceptions(true);
  CompileRun(source);
  v8::V8::SetCaptureStackTraceForUncaughtExceptions(false);
  v8::V8::RemoveMessageListeners(RethrowPrimitiveStackTraceHandler);
}


static void RethrowExistingStackTraceHandler(v8::Handle<v8::Message> message,
                                              v8::Handle<v8::Value> data) {
  // Use the frame where JavaScript is called from.
  v8::Handle<v8::StackTrace> stack_trace = message->GetStackTrace();
  CHECK(!stack_trace.IsEmpty());
  CHECK_EQ(1, stack_trace->GetFrameCount());
  CHECK_EQ(1, stack_trace->GetFrame(0)->GetLineNumber());
}


// Test that the stack trace is captured when the error object is created and
// not where it is thrown.
TEST(RethrowExistingStackTrace) {
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());
  const char* source =
      "var e = new Error();           \n"
      "throw e;                       \n";
  v8::V8::AddMessageListener(RethrowExistingStackTraceHandler);
  v8::V8::SetCaptureStackTraceForUncaughtExceptions(true);
  CompileRun(source);
  v8::V8::SetCaptureStackTraceForUncaughtExceptions(false);
  v8::V8::RemoveMessageListeners(RethrowExistingStackTraceHandler);
}


static void RethrowBogusErrorStackTraceHandler(v8::Handle<v8::Message> message,
                                               v8::Handle<v8::Value> data) {
  // Use the frame where JavaScript is called from.
  v8::Handle<v8::StackTrace> stack_trace = message->GetStackTrace();
  CHECK(!stack_trace.IsEmpty());
  CHECK_EQ(1, stack_trace->GetFrameCount());
  CHECK_EQ(2, stack_trace->GetFrame(0)->GetLineNumber());
}


// Test that the stack trace is captured where the bogus Error object is thrown.
TEST(RethrowBogusErrorStackTrace) {
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());
  const char* source =
      "var e = {__proto__: new Error()} \n"
      "throw e;                         \n";
  v8::V8::AddMessageListener(RethrowBogusErrorStackTraceHandler);
  v8::V8::SetCaptureStackTraceForUncaughtExceptions(true);
  CompileRun(source);
  v8::V8::SetCaptureStackTraceForUncaughtExceptions(false);
  v8::V8::RemoveMessageListeners(RethrowBogusErrorStackTraceHandler);
}


v8::PromiseRejectEvent reject_event = v8::kPromiseRejectWithNoHandler;
int promise_reject_counter = 0;
int promise_revoke_counter = 0;
int promise_reject_msg_line_number = -1;
int promise_reject_msg_column_number = -1;
int promise_reject_line_number = -1;
int promise_reject_column_number = -1;
int promise_reject_frame_count = -1;

void PromiseRejectCallback(v8::PromiseRejectMessage reject_message) {
  if (reject_message.GetEvent() == v8::kPromiseRejectWithNoHandler) {
    promise_reject_counter++;
    CcTest::global()->Set(v8_str("rejected"), reject_message.GetPromise());
    CcTest::global()->Set(v8_str("value"), reject_message.GetValue());
    v8::Handle<v8::Message> message =
        v8::Exception::CreateMessage(reject_message.GetValue());
    v8::Handle<v8::StackTrace> stack_trace = message->GetStackTrace();

    promise_reject_msg_line_number = message->GetLineNumber();
    promise_reject_msg_column_number = message->GetStartColumn() + 1;

    if (!stack_trace.IsEmpty()) {
      promise_reject_frame_count = stack_trace->GetFrameCount();
      if (promise_reject_frame_count > 0) {
        CHECK(stack_trace->GetFrame(0)->GetScriptName()->Equals(v8_str("pro")));
        promise_reject_line_number = stack_trace->GetFrame(0)->GetLineNumber();
        promise_reject_column_number = stack_trace->GetFrame(0)->GetColumn();
      } else {
        promise_reject_line_number = -1;
        promise_reject_column_number = -1;
      }
    }
  } else {
    promise_revoke_counter++;
    CcTest::global()->Set(v8_str("revoked"), reject_message.GetPromise());
    CHECK(reject_message.GetValue().IsEmpty());
  }
}


v8::Handle<v8::Promise> GetPromise(const char* name) {
  return v8::Handle<v8::Promise>::Cast(CcTest::global()->Get(v8_str(name)));
}


v8::Handle<v8::Value> RejectValue() {
  return CcTest::global()->Get(v8_str("value"));
}


void ResetPromiseStates() {
  promise_reject_counter = 0;
  promise_revoke_counter = 0;
  promise_reject_msg_line_number = -1;
  promise_reject_msg_column_number = -1;
  promise_reject_line_number = -1;
  promise_reject_column_number = -1;
  promise_reject_frame_count = -1;
  CcTest::global()->Set(v8_str("rejected"), v8_str(""));
  CcTest::global()->Set(v8_str("value"), v8_str(""));
  CcTest::global()->Set(v8_str("revoked"), v8_str(""));
}


TEST(PromiseRejectCallback) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);

  isolate->SetPromiseRejectCallback(PromiseRejectCallback);

  ResetPromiseStates();

  // Create promise p0.
  CompileRun(
      "var reject;            \n"
      "var p0 = new Promise(  \n"
      "  function(res, rej) { \n"
      "    reject = rej;      \n"
      "  }                    \n"
      ");                     \n");
  CHECK(!GetPromise("p0")->HasHandler());
  CHECK_EQ(0, promise_reject_counter);
  CHECK_EQ(0, promise_revoke_counter);

  // Add resolve handler (and default reject handler) to p0.
  CompileRun("var p1 = p0.then(function(){});");
  CHECK(GetPromise("p0")->HasHandler());
  CHECK(!GetPromise("p1")->HasHandler());
  CHECK_EQ(0, promise_reject_counter);
  CHECK_EQ(0, promise_revoke_counter);

  // Reject p0.
  CompileRun("reject('ppp');");
  CHECK(GetPromise("p0")->HasHandler());
  CHECK(!GetPromise("p1")->HasHandler());
  CHECK_EQ(1, promise_reject_counter);
  CHECK_EQ(0, promise_revoke_counter);
  CHECK_EQ(v8::kPromiseRejectWithNoHandler, reject_event);
  CHECK(GetPromise("rejected")->Equals(GetPromise("p1")));
  CHECK(RejectValue()->Equals(v8_str("ppp")));

  // Reject p0 again. Callback is not triggered again.
  CompileRun("reject();");
  CHECK(GetPromise("p0")->HasHandler());
  CHECK(!GetPromise("p1")->HasHandler());
  CHECK_EQ(1, promise_reject_counter);
  CHECK_EQ(0, promise_revoke_counter);

  // Add resolve handler to p1.
  CompileRun("var p2 = p1.then(function(){});");
  CHECK(GetPromise("p0")->HasHandler());
  CHECK(GetPromise("p1")->HasHandler());
  CHECK(!GetPromise("p2")->HasHandler());
  CHECK_EQ(2, promise_reject_counter);
  CHECK_EQ(1, promise_revoke_counter);
  CHECK(GetPromise("rejected")->Equals(GetPromise("p2")));
  CHECK(RejectValue()->Equals(v8_str("ppp")));
  CHECK(GetPromise("revoked")->Equals(GetPromise("p1")));

  ResetPromiseStates();

  // Create promise q0.
  CompileRun(
      "var q0 = new Promise(  \n"
      "  function(res, rej) { \n"
      "    reject = rej;      \n"
      "  }                    \n"
      ");                     \n");
  CHECK(!GetPromise("q0")->HasHandler());
  CHECK_EQ(0, promise_reject_counter);
  CHECK_EQ(0, promise_revoke_counter);

  // Add reject handler to q0.
  CompileRun("var q1 = q0.catch(function() {});");
  CHECK(GetPromise("q0")->HasHandler());
  CHECK(!GetPromise("q1")->HasHandler());
  CHECK_EQ(0, promise_reject_counter);
  CHECK_EQ(0, promise_revoke_counter);

  // Reject q0.
  CompileRun("reject('qq')");
  CHECK(GetPromise("q0")->HasHandler());
  CHECK(!GetPromise("q1")->HasHandler());
  CHECK_EQ(0, promise_reject_counter);
  CHECK_EQ(0, promise_revoke_counter);

  // Add a new reject handler, which rejects by returning Promise.reject().
  // The returned promise q_ triggers a reject callback at first, only to
  // revoke it when returning it causes q2 to be rejected.
  CompileRun(
      "var q_;"
      "var q2 = q0.catch(               \n"
      "   function() {                  \n"
      "     q_ = Promise.reject('qqq'); \n"
      "     return q_;                  \n"
      "   }                             \n"
      ");                               \n");
  CHECK(GetPromise("q0")->HasHandler());
  CHECK(!GetPromise("q1")->HasHandler());
  CHECK(!GetPromise("q2")->HasHandler());
  CHECK(GetPromise("q_")->HasHandler());
  CHECK_EQ(2, promise_reject_counter);
  CHECK_EQ(1, promise_revoke_counter);
  CHECK(GetPromise("rejected")->Equals(GetPromise("q2")));
  CHECK(GetPromise("revoked")->Equals(GetPromise("q_")));
  CHECK(RejectValue()->Equals(v8_str("qqq")));

  // Add a reject handler to the resolved q1, which rejects by throwing.
  CompileRun(
      "var q3 = q1.then(  \n"
      "   function() {    \n"
      "     throw 'qqqq'; \n"
      "   }               \n"
      ");                 \n");
  CHECK(GetPromise("q0")->HasHandler());
  CHECK(GetPromise("q1")->HasHandler());
  CHECK(!GetPromise("q2")->HasHandler());
  CHECK(!GetPromise("q3")->HasHandler());
  CHECK_EQ(3, promise_reject_counter);
  CHECK_EQ(1, promise_revoke_counter);
  CHECK(GetPromise("rejected")->Equals(GetPromise("q3")));
  CHECK(RejectValue()->Equals(v8_str("qqqq")));

  ResetPromiseStates();

  // Create promise r0, which has three handlers, two of which handle rejects.
  CompileRun(
      "var r0 = new Promise(             \n"
      "  function(res, rej) {            \n"
      "    reject = rej;                 \n"
      "  }                               \n"
      ");                                \n"
      "var r1 = r0.catch(function() {}); \n"
      "var r2 = r0.then(function() {});  \n"
      "var r3 = r0.then(function() {},   \n"
      "                 function() {});  \n");
  CHECK(GetPromise("r0")->HasHandler());
  CHECK(!GetPromise("r1")->HasHandler());
  CHECK(!GetPromise("r2")->HasHandler());
  CHECK(!GetPromise("r3")->HasHandler());
  CHECK_EQ(0, promise_reject_counter);
  CHECK_EQ(0, promise_revoke_counter);

  // Reject r0.
  CompileRun("reject('rrr')");
  CHECK(GetPromise("r0")->HasHandler());
  CHECK(!GetPromise("r1")->HasHandler());
  CHECK(!GetPromise("r2")->HasHandler());
  CHECK(!GetPromise("r3")->HasHandler());
  CHECK_EQ(1, promise_reject_counter);
  CHECK_EQ(0, promise_revoke_counter);
  CHECK(GetPromise("rejected")->Equals(GetPromise("r2")));
  CHECK(RejectValue()->Equals(v8_str("rrr")));

  // Add reject handler to r2.
  CompileRun("var r4 = r2.catch(function() {});");
  CHECK(GetPromise("r0")->HasHandler());
  CHECK(!GetPromise("r1")->HasHandler());
  CHECK(GetPromise("r2")->HasHandler());
  CHECK(!GetPromise("r3")->HasHandler());
  CHECK(!GetPromise("r4")->HasHandler());
  CHECK_EQ(1, promise_reject_counter);
  CHECK_EQ(1, promise_revoke_counter);
  CHECK(GetPromise("revoked")->Equals(GetPromise("r2")));
  CHECK(RejectValue()->Equals(v8_str("rrr")));

  // Add reject handlers to r4.
  CompileRun("var r5 = r4.then(function() {}, function() {});");
  CHECK(GetPromise("r0")->HasHandler());
  CHECK(!GetPromise("r1")->HasHandler());
  CHECK(GetPromise("r2")->HasHandler());
  CHECK(!GetPromise("r3")->HasHandler());
  CHECK(GetPromise("r4")->HasHandler());
  CHECK(!GetPromise("r5")->HasHandler());
  CHECK_EQ(1, promise_reject_counter);
  CHECK_EQ(1, promise_revoke_counter);

  ResetPromiseStates();

  // Create promise s0, which has three handlers, none of which handle rejects.
  CompileRun(
      "var s0 = new Promise(            \n"
      "  function(res, rej) {           \n"
      "    reject = rej;                \n"
      "  }                              \n"
      ");                               \n"
      "var s1 = s0.then(function() {}); \n"
      "var s2 = s0.then(function() {}); \n"
      "var s3 = s0.then(function() {}); \n");
  CHECK(GetPromise("s0")->HasHandler());
  CHECK(!GetPromise("s1")->HasHandler());
  CHECK(!GetPromise("s2")->HasHandler());
  CHECK(!GetPromise("s3")->HasHandler());
  CHECK_EQ(0, promise_reject_counter);
  CHECK_EQ(0, promise_revoke_counter);

  // Reject s0.
  CompileRun("reject('sss')");
  CHECK(GetPromise("s0")->HasHandler());
  CHECK(!GetPromise("s1")->HasHandler());
  CHECK(!GetPromise("s2")->HasHandler());
  CHECK(!GetPromise("s3")->HasHandler());
  CHECK_EQ(3, promise_reject_counter);
  CHECK_EQ(0, promise_revoke_counter);
  CHECK(RejectValue()->Equals(v8_str("sss")));

  // Test stack frames.
  V8::SetCaptureStackTraceForUncaughtExceptions(true);

  ResetPromiseStates();

  // Create promise t0, which is rejected in the constructor with an error.
  CompileRunWithOrigin(
      "var t0 = new Promise(  \n"
      "  function(res, rej) { \n"
      "    reference_error;   \n"
      "  }                    \n"
      ");                     \n",
      "pro", 0, 0);
  CHECK(!GetPromise("t0")->HasHandler());
  CHECK_EQ(1, promise_reject_counter);
  CHECK_EQ(0, promise_revoke_counter);
  CHECK_EQ(2, promise_reject_frame_count);
  CHECK_EQ(3, promise_reject_line_number);
  CHECK_EQ(5, promise_reject_column_number);
  CHECK_EQ(3, promise_reject_msg_line_number);
  CHECK_EQ(5, promise_reject_msg_column_number);

  ResetPromiseStates();

  // Create promise u0 and chain u1 to it, which is rejected via throw.
  CompileRunWithOrigin(
      "var u0 = Promise.resolve();        \n"
      "var u1 = u0.then(                  \n"
      "           function() {            \n"
      "             (function() {         \n"
      "                throw new Error(); \n"
      "              })();                \n"
      "           }                       \n"
      "         );                        \n",
      "pro", 0, 0);
  CHECK(GetPromise("u0")->HasHandler());
  CHECK(!GetPromise("u1")->HasHandler());
  CHECK_EQ(1, promise_reject_counter);
  CHECK_EQ(0, promise_revoke_counter);
  CHECK_EQ(2, promise_reject_frame_count);
  CHECK_EQ(5, promise_reject_line_number);
  CHECK_EQ(23, promise_reject_column_number);
  CHECK_EQ(5, promise_reject_msg_line_number);
  CHECK_EQ(23, promise_reject_msg_column_number);

  // Throw in u3, which handles u1's rejection.
  CompileRunWithOrigin(
      "function f() {                \n"
      "  return (function() {        \n"
      "    return new Error();       \n"
      "  })();                       \n"
      "}                             \n"
      "var u2 = Promise.reject(f()); \n"
      "var u3 = u1.catch(            \n"
      "           function() {       \n"
      "             return u2;       \n"
      "           }                  \n"
      "         );                   \n",
      "pro", 0, 0);
  CHECK(GetPromise("u0")->HasHandler());
  CHECK(GetPromise("u1")->HasHandler());
  CHECK(GetPromise("u2")->HasHandler());
  CHECK(!GetPromise("u3")->HasHandler());
  CHECK_EQ(3, promise_reject_counter);
  CHECK_EQ(2, promise_revoke_counter);
  CHECK_EQ(3, promise_reject_frame_count);
  CHECK_EQ(3, promise_reject_line_number);
  CHECK_EQ(12, promise_reject_column_number);
  CHECK_EQ(3, promise_reject_msg_line_number);
  CHECK_EQ(12, promise_reject_msg_column_number);

  ResetPromiseStates();

  // Create promise rejected promise v0, which is incorrectly handled by v1
  // via chaining cycle.
  CompileRunWithOrigin(
      "var v0 = Promise.reject(); \n"
      "var v1 = v0.catch(         \n"
      "           function() {    \n"
      "             return v1;    \n"
      "           }               \n"
      "         );                \n",
      "pro", 0, 0);
  CHECK(GetPromise("v0")->HasHandler());
  CHECK(!GetPromise("v1")->HasHandler());
  CHECK_EQ(2, promise_reject_counter);
  CHECK_EQ(1, promise_revoke_counter);
  CHECK_EQ(0, promise_reject_frame_count);
  CHECK_EQ(-1, promise_reject_line_number);
  CHECK_EQ(-1, promise_reject_column_number);

  ResetPromiseStates();

  // Create promise t1, which rejects by throwing syntax error from eval.
  CompileRunWithOrigin(
      "var t1 = new Promise(   \n"
      "  function(res, rej) {  \n"
      "    var content = '\\n\\\n"
      "      }';               \n"
      "    eval(content);      \n"
      "  }                     \n"
      ");                      \n",
      "pro", 0, 0);
  CHECK(!GetPromise("t1")->HasHandler());
  CHECK_EQ(1, promise_reject_counter);
  CHECK_EQ(0, promise_revoke_counter);
  CHECK_EQ(2, promise_reject_frame_count);
  CHECK_EQ(5, promise_reject_line_number);
  CHECK_EQ(10, promise_reject_column_number);
  CHECK_EQ(2, promise_reject_msg_line_number);
  CHECK_EQ(7, promise_reject_msg_column_number);
}


void AnalyzeStackOfEvalWithSourceURL(
    const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::HandleScope scope(args.GetIsolate());
  v8::Handle<v8::StackTrace> stackTrace = v8::StackTrace::CurrentStackTrace(
      args.GetIsolate(), 10, v8::StackTrace::kDetailed);
  CHECK_EQ(5, stackTrace->GetFrameCount());
  v8::Handle<v8::String> url = v8_str("eval_url");
  for (int i = 0; i < 3; i++) {
    v8::Handle<v8::String> name =
        stackTrace->GetFrame(i)->GetScriptNameOrSourceURL();
    CHECK(!name.IsEmpty());
    CHECK(url->Equals(name));
  }
}


TEST(SourceURLInStackTrace) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  Local<ObjectTemplate> templ = ObjectTemplate::New(isolate);
  templ->Set(v8_str("AnalyzeStackOfEvalWithSourceURL"),
             v8::FunctionTemplate::New(isolate,
                                       AnalyzeStackOfEvalWithSourceURL));
  LocalContext context(0, templ);

  const char *source =
    "function outer() {\n"
    "function bar() {\n"
    "  AnalyzeStackOfEvalWithSourceURL();\n"
    "}\n"
    "function foo() {\n"
    "\n"
    "  bar();\n"
    "}\n"
    "foo();\n"
    "}\n"
    "eval('(' + outer +')()%s');";

  i::ScopedVector<char> code(1024);
  i::SNPrintF(code, source, "//# sourceURL=eval_url");
  CHECK(CompileRun(code.start())->IsUndefined());
  i::SNPrintF(code, source, "//@ sourceURL=eval_url");
  CHECK(CompileRun(code.start())->IsUndefined());
}


static int scriptIdInStack[2];

void AnalyzeScriptIdInStack(
    const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::HandleScope scope(args.GetIsolate());
  v8::Handle<v8::StackTrace> stackTrace = v8::StackTrace::CurrentStackTrace(
      args.GetIsolate(), 10, v8::StackTrace::kScriptId);
  CHECK_EQ(2, stackTrace->GetFrameCount());
  for (int i = 0; i < 2; i++) {
    scriptIdInStack[i] = stackTrace->GetFrame(i)->GetScriptId();
  }
}


TEST(ScriptIdInStackTrace) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  Local<ObjectTemplate> templ = ObjectTemplate::New(isolate);
  templ->Set(v8_str("AnalyzeScriptIdInStack"),
             v8::FunctionTemplate::New(isolate, AnalyzeScriptIdInStack));
  LocalContext context(0, templ);

  v8::Handle<v8::String> scriptSource = v8::String::NewFromUtf8(
    isolate,
    "function foo() {\n"
    "  AnalyzeScriptIdInStack();"
    "}\n"
    "foo();\n");
  v8::Local<v8::Script> script = CompileWithOrigin(scriptSource, "test");
  script->Run();
  for (int i = 0; i < 2; i++) {
    CHECK(scriptIdInStack[i] != v8::Message::kNoScriptIdInfo);
    CHECK_EQ(scriptIdInStack[i], script->GetUnboundScript()->GetId());
  }
}


void AnalyzeStackOfInlineScriptWithSourceURL(
    const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::HandleScope scope(args.GetIsolate());
  v8::Handle<v8::StackTrace> stackTrace = v8::StackTrace::CurrentStackTrace(
      args.GetIsolate(), 10, v8::StackTrace::kDetailed);
  CHECK_EQ(4, stackTrace->GetFrameCount());
  v8::Handle<v8::String> url = v8_str("url");
  for (int i = 0; i < 3; i++) {
    v8::Handle<v8::String> name =
        stackTrace->GetFrame(i)->GetScriptNameOrSourceURL();
    CHECK(!name.IsEmpty());
    CHECK(url->Equals(name));
  }
}


TEST(InlineScriptWithSourceURLInStackTrace) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  Local<ObjectTemplate> templ = ObjectTemplate::New(isolate);
  templ->Set(v8_str("AnalyzeStackOfInlineScriptWithSourceURL"),
             v8::FunctionTemplate::New(
                 CcTest::isolate(), AnalyzeStackOfInlineScriptWithSourceURL));
  LocalContext context(0, templ);

  const char *source =
    "function outer() {\n"
    "function bar() {\n"
    "  AnalyzeStackOfInlineScriptWithSourceURL();\n"
    "}\n"
    "function foo() {\n"
    "\n"
    "  bar();\n"
    "}\n"
    "foo();\n"
    "}\n"
    "outer()\n%s";

  i::ScopedVector<char> code(1024);
  i::SNPrintF(code, source, "//# sourceURL=source_url");
  CHECK(CompileRunWithOrigin(code.start(), "url", 0, 1)->IsUndefined());
  i::SNPrintF(code, source, "//@ sourceURL=source_url");
  CHECK(CompileRunWithOrigin(code.start(), "url", 0, 1)->IsUndefined());
}


void AnalyzeStackOfDynamicScriptWithSourceURL(
    const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::HandleScope scope(args.GetIsolate());
  v8::Handle<v8::StackTrace> stackTrace = v8::StackTrace::CurrentStackTrace(
      args.GetIsolate(), 10, v8::StackTrace::kDetailed);
  CHECK_EQ(4, stackTrace->GetFrameCount());
  v8::Handle<v8::String> url = v8_str("source_url");
  for (int i = 0; i < 3; i++) {
    v8::Handle<v8::String> name =
        stackTrace->GetFrame(i)->GetScriptNameOrSourceURL();
    CHECK(!name.IsEmpty());
    CHECK(url->Equals(name));
  }
}


TEST(DynamicWithSourceURLInStackTrace) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  Local<ObjectTemplate> templ = ObjectTemplate::New(isolate);
  templ->Set(v8_str("AnalyzeStackOfDynamicScriptWithSourceURL"),
             v8::FunctionTemplate::New(
                 CcTest::isolate(), AnalyzeStackOfDynamicScriptWithSourceURL));
  LocalContext context(0, templ);

  const char *source =
    "function outer() {\n"
    "function bar() {\n"
    "  AnalyzeStackOfDynamicScriptWithSourceURL();\n"
    "}\n"
    "function foo() {\n"
    "\n"
    "  bar();\n"
    "}\n"
    "foo();\n"
    "}\n"
    "outer()\n%s";

  i::ScopedVector<char> code(1024);
  i::SNPrintF(code, source, "//# sourceURL=source_url");
  CHECK(CompileRunWithOrigin(code.start(), "url", 0, 0)->IsUndefined());
  i::SNPrintF(code, source, "//@ sourceURL=source_url");
  CHECK(CompileRunWithOrigin(code.start(), "url", 0, 0)->IsUndefined());
}


TEST(DynamicWithSourceURLInStackTraceString) {
  LocalContext context;
  v8::HandleScope scope(context->GetIsolate());

  const char *source =
    "function outer() {\n"
    "  function foo() {\n"
    "    FAIL.FAIL;\n"
    "  }\n"
    "  foo();\n"
    "}\n"
    "outer()\n%s";

  i::ScopedVector<char> code(1024);
  i::SNPrintF(code, source, "//# sourceURL=source_url");
  v8::TryCatch try_catch;
  CompileRunWithOrigin(code.start(), "", 0, 0);
  CHECK(try_catch.HasCaught());
  v8::String::Utf8Value stack(try_catch.StackTrace());
  CHECK(strstr(*stack, "at foo (source_url:3:5)") != NULL);
}


TEST(EvalWithSourceURLInMessageScriptResourceNameOrSourceURL) {
  LocalContext context;
  v8::HandleScope scope(context->GetIsolate());

  const char *source =
    "function outer() {\n"
    "  var scriptContents = \"function foo() { FAIL.FAIL; }\\\n"
    "  //# sourceURL=source_url\";\n"
    "  eval(scriptContents);\n"
    "  foo(); }\n"
    "outer();\n"
    "//# sourceURL=outer_url";

  v8::TryCatch try_catch;
  CompileRun(source);
  CHECK(try_catch.HasCaught());

  Local<v8::Message> message = try_catch.Message();
  Handle<Value> sourceURL =
    message->GetScriptOrigin().ResourceName();
  CHECK_EQ(0, strcmp(*v8::String::Utf8Value(sourceURL), "source_url"));
}


TEST(RecursionWithSourceURLInMessageScriptResourceNameOrSourceURL) {
  LocalContext context;
  v8::HandleScope scope(context->GetIsolate());

  const char *source =
    "function outer() {\n"
    "  var scriptContents = \"function boo(){ boo(); }\\\n"
    "  //# sourceURL=source_url\";\n"
    "  eval(scriptContents);\n"
    "  boo(); }\n"
    "outer();\n"
    "//# sourceURL=outer_url";

  v8::TryCatch try_catch;
  CompileRun(source);
  CHECK(try_catch.HasCaught());

  Local<v8::Message> message = try_catch.Message();
  Handle<Value> sourceURL =
    message->GetScriptOrigin().ResourceName();
  CHECK_EQ(0, strcmp(*v8::String::Utf8Value(sourceURL), "source_url"));
}


static void CreateGarbageInOldSpace() {
  i::Factory* factory = CcTest::i_isolate()->factory();
  v8::HandleScope scope(CcTest::isolate());
  i::AlwaysAllocateScope always_allocate(CcTest::i_isolate());
  for (int i = 0; i < 1000; i++) {
    factory->NewFixedArray(1000, i::TENURED);
  }
}


// Test that idle notification can be handled and eventually collects garbage.
TEST(TestIdleNotification) {
  const intptr_t MB = 1024 * 1024;
  const double IdlePauseInSeconds = 1.0;
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());
  intptr_t initial_size = CcTest::heap()->SizeOfObjects();
  CreateGarbageInOldSpace();
  intptr_t size_with_garbage = CcTest::heap()->SizeOfObjects();
  CHECK_GT(size_with_garbage, initial_size + MB);
  bool finished = false;
  for (int i = 0; i < 200 && !finished; i++) {
    finished = env->GetIsolate()->IdleNotificationDeadline(
        (v8::base::TimeTicks::HighResolutionNow().ToInternalValue() /
         static_cast<double>(v8::base::Time::kMicrosecondsPerSecond)) +
        IdlePauseInSeconds);
  }
  intptr_t final_size = CcTest::heap()->SizeOfObjects();
  CHECK(finished);
  CHECK_LT(final_size, initial_size + 1);
}


TEST(Regress2333) {
  LocalContext env;
  for (int i = 0; i < 3; i++) {
    CcTest::heap()->CollectGarbage(i::NEW_SPACE);
  }
}

static uint32_t* stack_limit;

static void GetStackLimitCallback(
    const v8::FunctionCallbackInfo<v8::Value>& args) {
  stack_limit = reinterpret_cast<uint32_t*>(
      CcTest::i_isolate()->stack_guard()->real_climit());
}


// Uses the address of a local variable to determine the stack top now.
// Given a size, returns an address that is that far from the current
// top of stack.
static uint32_t* ComputeStackLimit(uint32_t size) {
  uint32_t* answer = &size - (size / sizeof(size));
  // If the size is very large and the stack is very near the bottom of
  // memory then the calculation above may wrap around and give an address
  // that is above the (downwards-growing) stack.  In that case we return
  // a very low address.
  if (answer > &size) return reinterpret_cast<uint32_t*>(sizeof(size));
  return answer;
}


// We need at least 165kB for an x64 debug build with clang and ASAN.
static const int stack_breathing_room = 256 * i::KB;


TEST(SetStackLimit) {
  uint32_t* set_limit = ComputeStackLimit(stack_breathing_room);

  // Set stack limit.
  CcTest::isolate()->SetStackLimit(reinterpret_cast<uintptr_t>(set_limit));

  // Execute a script.
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());
  Local<v8::FunctionTemplate> fun_templ =
      v8::FunctionTemplate::New(env->GetIsolate(), GetStackLimitCallback);
  Local<Function> fun = fun_templ->GetFunction();
  env->Global()->Set(v8_str("get_stack_limit"), fun);
  CompileRun("get_stack_limit();");

  CHECK(stack_limit == set_limit);
}


TEST(SetStackLimitInThread) {
  uint32_t* set_limit;
  {
    v8::Locker locker(CcTest::isolate());
    set_limit = ComputeStackLimit(stack_breathing_room);

    // Set stack limit.
    CcTest::isolate()->SetStackLimit(reinterpret_cast<uintptr_t>(set_limit));

    // Execute a script.
    v8::HandleScope scope(CcTest::isolate());
    LocalContext env;
    Local<v8::FunctionTemplate> fun_templ =
        v8::FunctionTemplate::New(CcTest::isolate(), GetStackLimitCallback);
    Local<Function> fun = fun_templ->GetFunction();
    env->Global()->Set(v8_str("get_stack_limit"), fun);
    CompileRun("get_stack_limit();");

    CHECK(stack_limit == set_limit);
  }
  {
    v8::Locker locker(CcTest::isolate());
    CHECK(stack_limit == set_limit);
  }
}


THREADED_TEST(GetHeapStatistics) {
  LocalContext c1;
  v8::HandleScope scope(c1->GetIsolate());
  v8::HeapStatistics heap_statistics;
  CHECK_EQ(0u, heap_statistics.total_heap_size());
  CHECK_EQ(0u, heap_statistics.used_heap_size());
  c1->GetIsolate()->GetHeapStatistics(&heap_statistics);
  CHECK_NE(static_cast<int>(heap_statistics.total_heap_size()), 0);
  CHECK_NE(static_cast<int>(heap_statistics.used_heap_size()), 0);
}


class VisitorImpl : public v8::ExternalResourceVisitor {
 public:
  explicit VisitorImpl(TestResource** resource) {
    for (int i = 0; i < 4; i++) {
      resource_[i] = resource[i];
      found_resource_[i] = false;
    }
  }
  virtual ~VisitorImpl() {}
  virtual void VisitExternalString(v8::Handle<v8::String> string) {
    if (!string->IsExternal()) {
      CHECK(string->IsExternalOneByte());
      return;
    }
    v8::String::ExternalStringResource* resource =
        string->GetExternalStringResource();
    CHECK(resource);
    for (int i = 0; i < 4; i++) {
      if (resource_[i] == resource) {
        CHECK(!found_resource_[i]);
        found_resource_[i] = true;
      }
    }
  }
  void CheckVisitedResources() {
    for (int i = 0; i < 4; i++) {
      CHECK(found_resource_[i]);
    }
  }

 private:
  v8::String::ExternalStringResource* resource_[4];
  bool found_resource_[4];
};


TEST(ExternalizeOldSpaceTwoByteCons) {
  v8::Isolate* isolate = CcTest::isolate();
  LocalContext env;
  v8::HandleScope scope(isolate);
  v8::Local<v8::String> cons =
      CompileRun("'Romeo Montague ' + 'Juliet Capulet'")->ToString(isolate);
  CHECK(v8::Utils::OpenHandle(*cons)->IsConsString());
  CcTest::heap()->CollectAllAvailableGarbage();
  CHECK(CcTest::heap()->old_pointer_space()->Contains(
      *v8::Utils::OpenHandle(*cons)));

  TestResource* resource = new TestResource(
      AsciiToTwoByteString("Romeo Montague Juliet Capulet"));
  cons->MakeExternal(resource);

  CHECK(cons->IsExternal());
  CHECK_EQ(resource, cons->GetExternalStringResource());
  String::Encoding encoding;
  CHECK_EQ(resource, cons->GetExternalStringResourceBase(&encoding));
  CHECK_EQ(String::TWO_BYTE_ENCODING, encoding);
}


TEST(ExternalizeOldSpaceOneByteCons) {
  v8::Isolate* isolate = CcTest::isolate();
  LocalContext env;
  v8::HandleScope scope(isolate);
  v8::Local<v8::String> cons =
      CompileRun("'Romeo Montague ' + 'Juliet Capulet'")->ToString(isolate);
  CHECK(v8::Utils::OpenHandle(*cons)->IsConsString());
  CcTest::heap()->CollectAllAvailableGarbage();
  CHECK(CcTest::heap()->old_pointer_space()->Contains(
      *v8::Utils::OpenHandle(*cons)));

  TestOneByteResource* resource =
      new TestOneByteResource(i::StrDup("Romeo Montague Juliet Capulet"));
  cons->MakeExternal(resource);

  CHECK(cons->IsExternalOneByte());
  CHECK_EQ(resource, cons->GetExternalOneByteStringResource());
  String::Encoding encoding;
  CHECK_EQ(resource, cons->GetExternalStringResourceBase(&encoding));
  CHECK_EQ(String::ONE_BYTE_ENCODING, encoding);
}


TEST(VisitExternalStrings) {
  v8::Isolate* isolate = CcTest::isolate();
  LocalContext env;
  v8::HandleScope scope(isolate);
  const char* string = "Some string";
  uint16_t* two_byte_string = AsciiToTwoByteString(string);
  TestResource* resource[4];
  resource[0] = new TestResource(two_byte_string);
  v8::Local<v8::String> string0 =
      v8::String::NewExternal(env->GetIsolate(), resource[0]);
  resource[1] = new TestResource(two_byte_string, NULL, false);
  v8::Local<v8::String> string1 =
      v8::String::NewExternal(env->GetIsolate(), resource[1]);

  // Externalized symbol.
  resource[2] = new TestResource(two_byte_string, NULL, false);
  v8::Local<v8::String> string2 = v8::String::NewFromUtf8(
      env->GetIsolate(), string, v8::String::kInternalizedString);
  CHECK(string2->MakeExternal(resource[2]));

  // Symbolized External.
  resource[3] = new TestResource(AsciiToTwoByteString("Some other string"));
  v8::Local<v8::String> string3 =
      v8::String::NewExternal(env->GetIsolate(), resource[3]);
  CcTest::heap()->CollectAllAvailableGarbage();  // Tenure string.
  // Turn into a symbol.
  i::Handle<i::String> string3_i = v8::Utils::OpenHandle(*string3);
  CHECK(!CcTest::i_isolate()->factory()->InternalizeString(
      string3_i).is_null());
  CHECK(string3_i->IsInternalizedString());

  // We need to add usages for string* to avoid warnings in GCC 4.7
  CHECK(string0->IsExternal());
  CHECK(string1->IsExternal());
  CHECK(string2->IsExternal());
  CHECK(string3->IsExternal());

  VisitorImpl visitor(resource);
  v8::V8::VisitExternalResources(&visitor);
  visitor.CheckVisitedResources();
}


TEST(ExternalStringCollectedAtTearDown) {
  int destroyed = 0;
  v8::Isolate* isolate = v8::Isolate::New();
  { v8::Isolate::Scope isolate_scope(isolate);
    v8::HandleScope handle_scope(isolate);
    const char* s = "One string to test them all, one string to find them.";
    TestOneByteResource* inscription =
        new TestOneByteResource(i::StrDup(s), &destroyed);
    v8::Local<v8::String> ring = v8::String::NewExternal(isolate, inscription);
    // Ring is still alive.  Orcs are roaming freely across our lands.
    CHECK_EQ(0, destroyed);
    USE(ring);
  }

  isolate->Dispose();
  // Ring has been destroyed.  Free Peoples of Middle-earth Rejoice.
  CHECK_EQ(1, destroyed);
}


TEST(ExternalInternalizedStringCollectedAtTearDown) {
  int destroyed = 0;
  v8::Isolate* isolate = v8::Isolate::New();
  { v8::Isolate::Scope isolate_scope(isolate);
    LocalContext env(isolate);
    v8::HandleScope handle_scope(isolate);
    CompileRun("var ring = 'One string to test them all';");
    const char* s = "One string to test them all";
    TestOneByteResource* inscription =
        new TestOneByteResource(i::StrDup(s), &destroyed);
    v8::Local<v8::String> ring = CompileRun("ring")->ToString(isolate);
    CHECK(v8::Utils::OpenHandle(*ring)->IsInternalizedString());
    ring->MakeExternal(inscription);
    // Ring is still alive.  Orcs are roaming freely across our lands.
    CHECK_EQ(0, destroyed);
    USE(ring);
  }

  isolate->Dispose();
  // Ring has been destroyed.  Free Peoples of Middle-earth Rejoice.
  CHECK_EQ(1, destroyed);
}


TEST(ExternalInternalizedStringCollectedAtGC) {
  // TODO(mvstanton): vector ics need weak support.
  if (i::FLAG_vector_ics) return;

  int destroyed = 0;
  { LocalContext env;
    v8::HandleScope handle_scope(env->GetIsolate());
    CompileRun("var ring = 'One string to test them all';");
    const char* s = "One string to test them all";
    TestOneByteResource* inscription =
        new TestOneByteResource(i::StrDup(s), &destroyed);
    v8::Local<v8::String> ring = CompileRun("ring").As<v8::String>();
    CHECK(v8::Utils::OpenHandle(*ring)->IsInternalizedString());
    ring->MakeExternal(inscription);
    // Ring is still alive.  Orcs are roaming freely across our lands.
    CHECK_EQ(0, destroyed);
    USE(ring);
  }

  // Garbage collector deals swift blows to evil.
  CcTest::i_isolate()->compilation_cache()->Clear();
  CcTest::heap()->CollectAllAvailableGarbage();

  // Ring has been destroyed.  Free Peoples of Middle-earth Rejoice.
  CHECK_EQ(1, destroyed);
}


static double DoubleFromBits(uint64_t value) {
  double target;
  i::MemCopy(&target, &value, sizeof(target));
  return target;
}


static uint64_t DoubleToBits(double value) {
  uint64_t target;
  i::MemCopy(&target, &value, sizeof(target));
  return target;
}


static double DoubleToDateTime(double input) {
  double date_limit = 864e13;
  if (std::isnan(input) || input < -date_limit || input > date_limit) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  return (input < 0) ? -(std::floor(-input)) : std::floor(input);
}


// We don't have a consistent way to write 64-bit constants syntactically, so we
// split them into two 32-bit constants and combine them programmatically.
static double DoubleFromBits(uint32_t high_bits, uint32_t low_bits) {
  return DoubleFromBits((static_cast<uint64_t>(high_bits) << 32) | low_bits);
}


THREADED_TEST(QuietSignalingNaNs) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);
  v8::TryCatch try_catch;

  // Special double values.
  double snan = DoubleFromBits(0x7ff00000, 0x00000001);
  double qnan = DoubleFromBits(0x7ff80000, 0x00000000);
  double infinity = DoubleFromBits(0x7ff00000, 0x00000000);
  double max_normal = DoubleFromBits(0x7fefffff, 0xffffffffu);
  double min_normal = DoubleFromBits(0x00100000, 0x00000000);
  double max_denormal = DoubleFromBits(0x000fffff, 0xffffffffu);
  double min_denormal = DoubleFromBits(0x00000000, 0x00000001);

  // Date values are capped at +/-100000000 days (times 864e5 ms per day)
  // on either side of the epoch.
  double date_limit = 864e13;

  double test_values[] = {
      snan,
      qnan,
      infinity,
      max_normal,
      date_limit + 1,
      date_limit,
      min_normal,
      max_denormal,
      min_denormal,
      0,
      -0,
      -min_denormal,
      -max_denormal,
      -min_normal,
      -date_limit,
      -date_limit - 1,
      -max_normal,
      -infinity,
      -qnan,
      -snan
  };
  int num_test_values = 20;

  for (int i = 0; i < num_test_values; i++) {
    double test_value = test_values[i];

    // Check that Number::New preserves non-NaNs and quiets SNaNs.
    v8::Handle<v8::Value> number = v8::Number::New(isolate, test_value);
    double stored_number = number->NumberValue();
    if (!std::isnan(test_value)) {
      CHECK_EQ(test_value, stored_number);
    } else {
      uint64_t stored_bits = DoubleToBits(stored_number);
      // Check if quiet nan (bits 51..62 all set).
#if (defined(V8_TARGET_ARCH_MIPS) || defined(V8_TARGET_ARCH_MIPS64)) && \
    !defined(_MIPS_ARCH_MIPS64R6) && !defined(USE_SIMULATOR)
      // Most significant fraction bit for quiet nan is set to 0
      // on MIPS architecture. Allowed by IEEE-754.
      CHECK_EQ(0xffe, static_cast<int>((stored_bits >> 51) & 0xfff));
#else
      CHECK_EQ(0xfff, static_cast<int>((stored_bits >> 51) & 0xfff));
#endif
    }

    // Check that Date::New preserves non-NaNs in the date range and
    // quiets SNaNs.
    v8::Handle<v8::Value> date =
        v8::Date::New(isolate, test_value);
    double expected_stored_date = DoubleToDateTime(test_value);
    double stored_date = date->NumberValue();
    if (!std::isnan(expected_stored_date)) {
      CHECK_EQ(expected_stored_date, stored_date);
    } else {
      uint64_t stored_bits = DoubleToBits(stored_date);
      // Check if quiet nan (bits 51..62 all set).
#if (defined(V8_TARGET_ARCH_MIPS) || defined(V8_TARGET_ARCH_MIPS64)) && \
    !defined(_MIPS_ARCH_MIPS64R6) && !defined(USE_SIMULATOR)
      // Most significant fraction bit for quiet nan is set to 0
      // on MIPS architecture. Allowed by IEEE-754.
      CHECK_EQ(0xffe, static_cast<int>((stored_bits >> 51) & 0xfff));
#else
      CHECK_EQ(0xfff, static_cast<int>((stored_bits >> 51) & 0xfff));
#endif
    }
  }
}


static void SpaghettiIncident(
    const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::HandleScope scope(args.GetIsolate());
  v8::TryCatch tc;
  v8::Handle<v8::String> str(args[0]->ToString(args.GetIsolate()));
  USE(str);
  if (tc.HasCaught())
    tc.ReThrow();
}


// Test that an exception can be propagated down through a spaghetti
// stack using ReThrow.
THREADED_TEST(SpaghettiStackReThrow) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  LocalContext context;
  context->Global()->Set(
      v8::String::NewFromUtf8(isolate, "s"),
      v8::FunctionTemplate::New(isolate, SpaghettiIncident)->GetFunction());
  v8::TryCatch try_catch;
  CompileRun(
      "var i = 0;"
      "var o = {"
      "  toString: function () {"
      "    if (i == 10) {"
      "      throw 'Hey!';"
      "    } else {"
      "      i++;"
      "      return s(o);"
      "    }"
      "  }"
      "};"
      "s(o);");
  CHECK(try_catch.HasCaught());
  v8::String::Utf8Value value(try_catch.Exception());
  CHECK_EQ(0, strcmp(*value, "Hey!"));
}


TEST(Regress528) {
  v8::V8::Initialize();
  v8::Isolate* isolate = CcTest::isolate();
  i::FLAG_retain_maps_for_n_gc = 0;
  v8::HandleScope scope(isolate);
  v8::Local<Context> other_context;
  int gc_count;

  // Create a context used to keep the code from aging in the compilation
  // cache.
  other_context = Context::New(isolate);

  // Context-dependent context data creates reference from the compilation
  // cache to the global object.
  const char* source_simple = "1";
  {
    v8::HandleScope scope(isolate);
    v8::Local<Context> context = Context::New(isolate);

    context->Enter();
    Local<v8::String> obj = v8::String::NewFromUtf8(isolate, "");
    context->SetEmbedderData(0, obj);
    CompileRun(source_simple);
    context->Exit();
  }
  isolate->ContextDisposedNotification();
  for (gc_count = 1; gc_count < 10; gc_count++) {
    other_context->Enter();
    CompileRun(source_simple);
    other_context->Exit();
    CcTest::heap()->CollectAllGarbage(i::Heap::kNoGCFlags);
    if (GetGlobalObjectsCount() == 1) break;
  }
  CHECK_GE(2, gc_count);
  CHECK_EQ(1, GetGlobalObjectsCount());

  // Eval in a function creates reference from the compilation cache to the
  // global object.
  const char* source_eval = "function f(){eval('1')}; f()";
  {
    v8::HandleScope scope(isolate);
    v8::Local<Context> context = Context::New(isolate);

    context->Enter();
    CompileRun(source_eval);
    context->Exit();
  }
  isolate->ContextDisposedNotification();
  for (gc_count = 1; gc_count < 10; gc_count++) {
    other_context->Enter();
    CompileRun(source_eval);
    other_context->Exit();
    CcTest::heap()->CollectAllGarbage(i::Heap::kNoGCFlags);
    if (GetGlobalObjectsCount() == 1) break;
  }
  CHECK_GE(2, gc_count);
  CHECK_EQ(1, GetGlobalObjectsCount());

  // Looking up the line number for an exception creates reference from the
  // compilation cache to the global object.
  const char* source_exception = "function f(){throw 1;} f()";
  {
    v8::HandleScope scope(isolate);
    v8::Local<Context> context = Context::New(isolate);

    context->Enter();
    v8::TryCatch try_catch;
    CompileRun(source_exception);
    CHECK(try_catch.HasCaught());
    v8::Handle<v8::Message> message = try_catch.Message();
    CHECK(!message.IsEmpty());
    CHECK_EQ(1, message->GetLineNumber());
    context->Exit();
  }
  isolate->ContextDisposedNotification();
  for (gc_count = 1; gc_count < 10; gc_count++) {
    other_context->Enter();
    CompileRun(source_exception);
    other_context->Exit();
    CcTest::heap()->CollectAllGarbage(i::Heap::kNoGCFlags);
    if (GetGlobalObjectsCount() == 1) break;
  }
  CHECK_GE(2, gc_count);
  CHECK_EQ(1, GetGlobalObjectsCount());

  isolate->ContextDisposedNotification();
}


THREADED_TEST(ScriptOrigin) {
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());
  v8::ScriptOrigin origin = v8::ScriptOrigin(
      v8::String::NewFromUtf8(env->GetIsolate(), "test"),
      v8::Integer::New(env->GetIsolate(), 1),
      v8::Integer::New(env->GetIsolate(), 1), v8::True(env->GetIsolate()),
      v8::Handle<v8::Integer>(), v8::True(env->GetIsolate()),
      v8::String::NewFromUtf8(env->GetIsolate(), "http://sourceMapUrl"));
  v8::Handle<v8::String> script = v8::String::NewFromUtf8(
      env->GetIsolate(), "function f() {}\n\nfunction g() {}");
  v8::Script::Compile(script, &origin)->Run();
  v8::Local<v8::Function> f = v8::Local<v8::Function>::Cast(
      env->Global()->Get(v8::String::NewFromUtf8(env->GetIsolate(), "f")));
  v8::Local<v8::Function> g = v8::Local<v8::Function>::Cast(
      env->Global()->Get(v8::String::NewFromUtf8(env->GetIsolate(), "g")));

  v8::ScriptOrigin script_origin_f = f->GetScriptOrigin();
  CHECK_EQ(0, strcmp("test",
                     *v8::String::Utf8Value(script_origin_f.ResourceName())));
  CHECK_EQ(1, script_origin_f.ResourceLineOffset()->Int32Value());
  CHECK(script_origin_f.ResourceIsSharedCrossOrigin()->Value());
  CHECK(script_origin_f.ResourceIsEmbedderDebugScript()->Value());
  printf("is name = %d\n", script_origin_f.SourceMapUrl()->IsUndefined());

  CHECK_EQ(0, strcmp("http://sourceMapUrl",
                     *v8::String::Utf8Value(script_origin_f.SourceMapUrl())));

  v8::ScriptOrigin script_origin_g = g->GetScriptOrigin();
  CHECK_EQ(0, strcmp("test",
                     *v8::String::Utf8Value(script_origin_g.ResourceName())));
  CHECK_EQ(1, script_origin_g.ResourceLineOffset()->Int32Value());
  CHECK(script_origin_g.ResourceIsSharedCrossOrigin()->Value());
  CHECK(script_origin_g.ResourceIsEmbedderDebugScript()->Value());
  CHECK_EQ(0, strcmp("http://sourceMapUrl",
                     *v8::String::Utf8Value(script_origin_g.SourceMapUrl())));
}


THREADED_TEST(FunctionGetInferredName) {
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());
  v8::ScriptOrigin origin =
      v8::ScriptOrigin(v8::String::NewFromUtf8(env->GetIsolate(), "test"));
  v8::Handle<v8::String> script = v8::String::NewFromUtf8(
      env->GetIsolate(),
      "var foo = { bar : { baz : function() {}}}; var f = foo.bar.baz;");
  v8::Script::Compile(script, &origin)->Run();
  v8::Local<v8::Function> f = v8::Local<v8::Function>::Cast(
      env->Global()->Get(v8::String::NewFromUtf8(env->GetIsolate(), "f")));
  CHECK_EQ(0,
           strcmp("foo.bar.baz", *v8::String::Utf8Value(f->GetInferredName())));
}


THREADED_TEST(FunctionGetDisplayName) {
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());
  const char* code = "var error = false;"
                     "function a() { this.x = 1; };"
                     "a.displayName = 'display_a';"
                     "var b = (function() {"
                     "  var f = function() { this.x = 2; };"
                     "  f.displayName = 'display_b';"
                     "  return f;"
                     "})();"
                     "var c = function() {};"
                     "c.__defineGetter__('displayName', function() {"
                     "  error = true;"
                     "  throw new Error();"
                     "});"
                     "function d() {};"
                     "d.__defineGetter__('displayName', function() {"
                     "  error = true;"
                     "  return 'wrong_display_name';"
                     "});"
                     "function e() {};"
                     "e.displayName = 'wrong_display_name';"
                     "e.__defineSetter__('displayName', function() {"
                     "  error = true;"
                     "  throw new Error();"
                     "});"
                     "function f() {};"
                     "f.displayName = { 'foo': 6, toString: function() {"
                     "  error = true;"
                     "  return 'wrong_display_name';"
                     "}};"
                     "var g = function() {"
                     "  arguments.callee.displayName = 'set_in_runtime';"
                     "}; g();"
                     ;
  v8::ScriptOrigin origin =
      v8::ScriptOrigin(v8::String::NewFromUtf8(env->GetIsolate(), "test"));
  v8::Script::Compile(v8::String::NewFromUtf8(env->GetIsolate(), code), &origin)
      ->Run();
  v8::Local<v8::Value> error =
      env->Global()->Get(v8::String::NewFromUtf8(env->GetIsolate(), "error"));
  v8::Local<v8::Function> a = v8::Local<v8::Function>::Cast(
      env->Global()->Get(v8::String::NewFromUtf8(env->GetIsolate(), "a")));
  v8::Local<v8::Function> b = v8::Local<v8::Function>::Cast(
      env->Global()->Get(v8::String::NewFromUtf8(env->GetIsolate(), "b")));
  v8::Local<v8::Function> c = v8::Local<v8::Function>::Cast(
      env->Global()->Get(v8::String::NewFromUtf8(env->GetIsolate(), "c")));
  v8::Local<v8::Function> d = v8::Local<v8::Function>::Cast(
      env->Global()->Get(v8::String::NewFromUtf8(env->GetIsolate(), "d")));
  v8::Local<v8::Function> e = v8::Local<v8::Function>::Cast(
      env->Global()->Get(v8::String::NewFromUtf8(env->GetIsolate(), "e")));
  v8::Local<v8::Function> f = v8::Local<v8::Function>::Cast(
      env->Global()->Get(v8::String::NewFromUtf8(env->GetIsolate(), "f")));
  v8::Local<v8::Function> g = v8::Local<v8::Function>::Cast(
      env->Global()->Get(v8::String::NewFromUtf8(env->GetIsolate(), "g")));
  CHECK_EQ(false, error->BooleanValue());
  CHECK_EQ(0, strcmp("display_a", *v8::String::Utf8Value(a->GetDisplayName())));
  CHECK_EQ(0, strcmp("display_b", *v8::String::Utf8Value(b->GetDisplayName())));
  CHECK(c->GetDisplayName()->IsUndefined());
  CHECK(d->GetDisplayName()->IsUndefined());
  CHECK(e->GetDisplayName()->IsUndefined());
  CHECK(f->GetDisplayName()->IsUndefined());
  CHECK_EQ(
      0, strcmp("set_in_runtime", *v8::String::Utf8Value(g->GetDisplayName())));
}


THREADED_TEST(ScriptLineNumber) {
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());
  v8::ScriptOrigin origin =
      v8::ScriptOrigin(v8::String::NewFromUtf8(env->GetIsolate(), "test"));
  v8::Handle<v8::String> script = v8::String::NewFromUtf8(
      env->GetIsolate(), "function f() {}\n\nfunction g() {}");
  v8::Script::Compile(script, &origin)->Run();
  v8::Local<v8::Function> f = v8::Local<v8::Function>::Cast(
      env->Global()->Get(v8::String::NewFromUtf8(env->GetIsolate(), "f")));
  v8::Local<v8::Function> g = v8::Local<v8::Function>::Cast(
      env->Global()->Get(v8::String::NewFromUtf8(env->GetIsolate(), "g")));
  CHECK_EQ(0, f->GetScriptLineNumber());
  CHECK_EQ(2, g->GetScriptLineNumber());
}


THREADED_TEST(ScriptColumnNumber) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);
  v8::ScriptOrigin origin =
      v8::ScriptOrigin(v8::String::NewFromUtf8(isolate, "test"),
                       v8::Integer::New(isolate, 3),
                       v8::Integer::New(isolate, 2));
  v8::Handle<v8::String> script = v8::String::NewFromUtf8(
      isolate, "function foo() {}\n\n     function bar() {}");
  v8::Script::Compile(script, &origin)->Run();
  v8::Local<v8::Function> foo = v8::Local<v8::Function>::Cast(
      env->Global()->Get(v8::String::NewFromUtf8(isolate, "foo")));
  v8::Local<v8::Function> bar = v8::Local<v8::Function>::Cast(
      env->Global()->Get(v8::String::NewFromUtf8(isolate, "bar")));
  CHECK_EQ(14, foo->GetScriptColumnNumber());
  CHECK_EQ(17, bar->GetScriptColumnNumber());
}


THREADED_TEST(FunctionIsBuiltin) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);
  v8::Local<v8::Function> f;
  f = v8::Local<v8::Function>::Cast(CompileRun("Math.floor"));
  CHECK(f->IsBuiltin());
  f = v8::Local<v8::Function>::Cast(CompileRun("Object"));
  CHECK(f->IsBuiltin());
  f = v8::Local<v8::Function>::Cast(CompileRun("Object.__defineSetter__"));
  CHECK(f->IsBuiltin());
  f = v8::Local<v8::Function>::Cast(CompileRun("Array.prototype.toString"));
  CHECK(f->IsBuiltin());
  f = v8::Local<v8::Function>::Cast(CompileRun("function a() {}; a;"));
  CHECK(!f->IsBuiltin());
}


THREADED_TEST(FunctionGetScriptId) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);
  v8::ScriptOrigin origin =
      v8::ScriptOrigin(v8::String::NewFromUtf8(isolate, "test"),
                       v8::Integer::New(isolate, 3),
                       v8::Integer::New(isolate, 2));
  v8::Handle<v8::String> scriptSource = v8::String::NewFromUtf8(
      isolate, "function foo() {}\n\n     function bar() {}");
  v8::Local<v8::Script> script(v8::Script::Compile(scriptSource, &origin));
  script->Run();
  v8::Local<v8::Function> foo = v8::Local<v8::Function>::Cast(
      env->Global()->Get(v8::String::NewFromUtf8(isolate, "foo")));
  v8::Local<v8::Function> bar = v8::Local<v8::Function>::Cast(
      env->Global()->Get(v8::String::NewFromUtf8(isolate, "bar")));
  CHECK_EQ(script->GetUnboundScript()->GetId(), foo->ScriptId());
  CHECK_EQ(script->GetUnboundScript()->GetId(), bar->ScriptId());
}


THREADED_TEST(FunctionGetBoundFunction) {
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());
  v8::ScriptOrigin origin = v8::ScriptOrigin(v8::String::NewFromUtf8(
      env->GetIsolate(), "test"));
  v8::Handle<v8::String> script = v8::String::NewFromUtf8(
      env->GetIsolate(),
      "var a = new Object();\n"
      "a.x = 1;\n"
      "function f () { return this.x };\n"
      "var g = f.bind(a);\n"
      "var b = g();");
  v8::Script::Compile(script, &origin)->Run();
  v8::Local<v8::Function> f = v8::Local<v8::Function>::Cast(
      env->Global()->Get(v8::String::NewFromUtf8(env->GetIsolate(), "f")));
  v8::Local<v8::Function> g = v8::Local<v8::Function>::Cast(
      env->Global()->Get(v8::String::NewFromUtf8(env->GetIsolate(), "g")));
  CHECK(g->GetBoundFunction()->IsFunction());
  Local<v8::Function> original_function = Local<v8::Function>::Cast(
      g->GetBoundFunction());
  CHECK(f->GetName()->Equals(original_function->GetName()));
  CHECK_EQ(f->GetScriptLineNumber(), original_function->GetScriptLineNumber());
  CHECK_EQ(f->GetScriptColumnNumber(),
           original_function->GetScriptColumnNumber());
}


static void GetterWhichReturns42(
    Local<String> name,
    const v8::PropertyCallbackInfo<v8::Value>& info) {
  CHECK(v8::Utils::OpenHandle(*info.This())->IsJSObject());
  CHECK(v8::Utils::OpenHandle(*info.Holder())->IsJSObject());
  info.GetReturnValue().Set(v8_num(42));
}


static void SetterWhichSetsYOnThisTo23(
    Local<String> name,
    Local<Value> value,
    const v8::PropertyCallbackInfo<void>& info) {
  CHECK(v8::Utils::OpenHandle(*info.This())->IsJSObject());
  CHECK(v8::Utils::OpenHandle(*info.Holder())->IsJSObject());
  Local<Object>::Cast(info.This())->Set(v8_str("y"), v8_num(23));
}


void FooGetInterceptor(Local<Name> name,
                       const v8::PropertyCallbackInfo<v8::Value>& info) {
  CHECK(v8::Utils::OpenHandle(*info.This())->IsJSObject());
  CHECK(v8::Utils::OpenHandle(*info.Holder())->IsJSObject());
  if (!name->Equals(v8_str("foo"))) return;
  info.GetReturnValue().Set(v8_num(42));
}


void FooSetInterceptor(Local<Name> name, Local<Value> value,
                       const v8::PropertyCallbackInfo<v8::Value>& info) {
  CHECK(v8::Utils::OpenHandle(*info.This())->IsJSObject());
  CHECK(v8::Utils::OpenHandle(*info.Holder())->IsJSObject());
  if (!name->Equals(v8_str("foo"))) return;
  Local<Object>::Cast(info.This())->Set(v8_str("y"), v8_num(23));
  info.GetReturnValue().Set(v8_num(23));
}


TEST(SetterOnConstructorPrototype) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  Local<ObjectTemplate> templ = ObjectTemplate::New(isolate);
  templ->SetAccessor(v8_str("x"), GetterWhichReturns42,
                     SetterWhichSetsYOnThisTo23);
  LocalContext context;
  context->Global()->Set(v8_str("P"), templ->NewInstance());
  CompileRun("function C1() {"
             "  this.x = 23;"
             "};"
             "C1.prototype = P;"
             "function C2() {"
             "  this.x = 23"
             "};"
             "C2.prototype = { };"
             "C2.prototype.__proto__ = P;");

  v8::Local<v8::Script> script;
  script = v8_compile("new C1();");
  for (int i = 0; i < 10; i++) {
    v8::Handle<v8::Object> c1 = v8::Handle<v8::Object>::Cast(script->Run());
    CHECK_EQ(42, c1->Get(v8_str("x"))->Int32Value());
    CHECK_EQ(23, c1->Get(v8_str("y"))->Int32Value());
  }

script = v8_compile("new C2();");
  for (int i = 0; i < 10; i++) {
    v8::Handle<v8::Object> c2 = v8::Handle<v8::Object>::Cast(script->Run());
    CHECK_EQ(42, c2->Get(v8_str("x"))->Int32Value());
    CHECK_EQ(23, c2->Get(v8_str("y"))->Int32Value());
  }
}


static void NamedPropertyGetterWhichReturns42(
    Local<Name> name, const v8::PropertyCallbackInfo<v8::Value>& info) {
  info.GetReturnValue().Set(v8_num(42));
}


static void NamedPropertySetterWhichSetsYOnThisTo23(
    Local<Name> name, Local<Value> value,
    const v8::PropertyCallbackInfo<v8::Value>& info) {
  if (name->Equals(v8_str("x"))) {
    Local<Object>::Cast(info.This())->Set(v8_str("y"), v8_num(23));
  }
}


THREADED_TEST(InterceptorOnConstructorPrototype) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  Local<ObjectTemplate> templ = ObjectTemplate::New(isolate);
  templ->SetHandler(v8::NamedPropertyHandlerConfiguration(
      NamedPropertyGetterWhichReturns42,
      NamedPropertySetterWhichSetsYOnThisTo23));
  LocalContext context;
  context->Global()->Set(v8_str("P"), templ->NewInstance());
  CompileRun("function C1() {"
             "  this.x = 23;"
             "};"
             "C1.prototype = P;"
             "function C2() {"
             "  this.x = 23"
             "};"
             "C2.prototype = { };"
             "C2.prototype.__proto__ = P;");

  v8::Local<v8::Script> script;
  script = v8_compile("new C1();");
  for (int i = 0; i < 10; i++) {
    v8::Handle<v8::Object> c1 = v8::Handle<v8::Object>::Cast(script->Run());
    CHECK_EQ(23, c1->Get(v8_str("x"))->Int32Value());
    CHECK_EQ(42, c1->Get(v8_str("y"))->Int32Value());
  }

  script = v8_compile("new C2();");
  for (int i = 0; i < 10; i++) {
    v8::Handle<v8::Object> c2 = v8::Handle<v8::Object>::Cast(script->Run());
    CHECK_EQ(23, c2->Get(v8_str("x"))->Int32Value());
    CHECK_EQ(42, c2->Get(v8_str("y"))->Int32Value());
  }
}


TEST(Regress618) {
  const char* source = "function C1() {"
                       "  this.x = 23;"
                       "};"
                       "C1.prototype = P;";

  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);
  v8::Local<v8::Script> script;

  // Use a simple object as prototype.
  v8::Local<v8::Object> prototype = v8::Object::New(isolate);
  prototype->Set(v8_str("y"), v8_num(42));
  context->Global()->Set(v8_str("P"), prototype);

  // This compile will add the code to the compilation cache.
  CompileRun(source);

  script = v8_compile("new C1();");
  // Allow enough iterations for the inobject slack tracking logic
  // to finalize instance size and install the fast construct stub.
  for (int i = 0; i < 256; i++) {
    v8::Handle<v8::Object> c1 = v8::Handle<v8::Object>::Cast(script->Run());
    CHECK_EQ(23, c1->Get(v8_str("x"))->Int32Value());
    CHECK_EQ(42, c1->Get(v8_str("y"))->Int32Value());
  }

  // Use an API object with accessors as prototype.
  Local<ObjectTemplate> templ = ObjectTemplate::New(isolate);
  templ->SetAccessor(v8_str("x"), GetterWhichReturns42,
                     SetterWhichSetsYOnThisTo23);
  context->Global()->Set(v8_str("P"), templ->NewInstance());

  // This compile will get the code from the compilation cache.
  CompileRun(source);

  script = v8_compile("new C1();");
  for (int i = 0; i < 10; i++) {
    v8::Handle<v8::Object> c1 = v8::Handle<v8::Object>::Cast(script->Run());
    CHECK_EQ(42, c1->Get(v8_str("x"))->Int32Value());
    CHECK_EQ(23, c1->Get(v8_str("y"))->Int32Value());
  }
}

v8::Isolate* gc_callbacks_isolate = NULL;
int prologue_call_count = 0;
int epilogue_call_count = 0;
int prologue_call_count_second = 0;
int epilogue_call_count_second = 0;
int prologue_call_count_alloc = 0;
int epilogue_call_count_alloc = 0;

void PrologueCallback(v8::GCType, v8::GCCallbackFlags flags) {
  CHECK_EQ(flags, v8::kNoGCCallbackFlags);
  ++prologue_call_count;
}


void PrologueCallback(v8::Isolate* isolate,
                      v8::GCType,
                      v8::GCCallbackFlags flags) {
  CHECK_EQ(flags, v8::kNoGCCallbackFlags);
  CHECK_EQ(gc_callbacks_isolate, isolate);
  ++prologue_call_count;
}


void EpilogueCallback(v8::GCType, v8::GCCallbackFlags flags) {
  CHECK_EQ(flags, v8::kNoGCCallbackFlags);
  ++epilogue_call_count;
}


void EpilogueCallback(v8::Isolate* isolate,
                      v8::GCType,
                      v8::GCCallbackFlags flags) {
  CHECK_EQ(flags, v8::kNoGCCallbackFlags);
  CHECK_EQ(gc_callbacks_isolate, isolate);
  ++epilogue_call_count;
}


void PrologueCallbackSecond(v8::GCType, v8::GCCallbackFlags flags) {
  CHECK_EQ(flags, v8::kNoGCCallbackFlags);
  ++prologue_call_count_second;
}


void PrologueCallbackSecond(v8::Isolate* isolate,
                            v8::GCType,
                            v8::GCCallbackFlags flags) {
  CHECK_EQ(flags, v8::kNoGCCallbackFlags);
  CHECK_EQ(gc_callbacks_isolate, isolate);
  ++prologue_call_count_second;
}


void EpilogueCallbackSecond(v8::GCType, v8::GCCallbackFlags flags) {
  CHECK_EQ(flags, v8::kNoGCCallbackFlags);
  ++epilogue_call_count_second;
}


void EpilogueCallbackSecond(v8::Isolate* isolate,
                            v8::GCType,
                            v8::GCCallbackFlags flags) {
  CHECK_EQ(flags, v8::kNoGCCallbackFlags);
  CHECK_EQ(gc_callbacks_isolate, isolate);
  ++epilogue_call_count_second;
}


void PrologueCallbackAlloc(v8::Isolate* isolate,
                           v8::GCType,
                           v8::GCCallbackFlags flags) {
  v8::HandleScope scope(isolate);

  CHECK_EQ(flags, v8::kNoGCCallbackFlags);
  CHECK_EQ(gc_callbacks_isolate, isolate);
  ++prologue_call_count_alloc;

  // Simulate full heap to see if we will reenter this callback
  SimulateFullSpace(CcTest::heap()->new_space());

  Local<Object> obj = Object::New(isolate);
  CHECK(!obj.IsEmpty());

  CcTest::heap()->CollectAllGarbage(
      i::Heap::kAbortIncrementalMarkingMask);
}


void EpilogueCallbackAlloc(v8::Isolate* isolate,
                           v8::GCType,
                           v8::GCCallbackFlags flags) {
  v8::HandleScope scope(isolate);

  CHECK_EQ(flags, v8::kNoGCCallbackFlags);
  CHECK_EQ(gc_callbacks_isolate, isolate);
  ++epilogue_call_count_alloc;

  // Simulate full heap to see if we will reenter this callback
  SimulateFullSpace(CcTest::heap()->new_space());

  Local<Object> obj = Object::New(isolate);
  CHECK(!obj.IsEmpty());

  CcTest::heap()->CollectAllGarbage(
      i::Heap::kAbortIncrementalMarkingMask);
}


TEST(GCCallbacksOld) {
  LocalContext context;

  v8::V8::AddGCPrologueCallback(PrologueCallback);
  v8::V8::AddGCEpilogueCallback(EpilogueCallback);
  CHECK_EQ(0, prologue_call_count);
  CHECK_EQ(0, epilogue_call_count);
  CcTest::heap()->CollectAllGarbage(i::Heap::kNoGCFlags);
  CHECK_EQ(1, prologue_call_count);
  CHECK_EQ(1, epilogue_call_count);
  v8::V8::AddGCPrologueCallback(PrologueCallbackSecond);
  v8::V8::AddGCEpilogueCallback(EpilogueCallbackSecond);
  CcTest::heap()->CollectAllGarbage(i::Heap::kNoGCFlags);
  CHECK_EQ(2, prologue_call_count);
  CHECK_EQ(2, epilogue_call_count);
  CHECK_EQ(1, prologue_call_count_second);
  CHECK_EQ(1, epilogue_call_count_second);
  v8::V8::RemoveGCPrologueCallback(PrologueCallback);
  v8::V8::RemoveGCEpilogueCallback(EpilogueCallback);
  CcTest::heap()->CollectAllGarbage(i::Heap::kNoGCFlags);
  CHECK_EQ(2, prologue_call_count);
  CHECK_EQ(2, epilogue_call_count);
  CHECK_EQ(2, prologue_call_count_second);
  CHECK_EQ(2, epilogue_call_count_second);
  v8::V8::RemoveGCPrologueCallback(PrologueCallbackSecond);
  v8::V8::RemoveGCEpilogueCallback(EpilogueCallbackSecond);
  CcTest::heap()->CollectAllGarbage(i::Heap::kNoGCFlags);
  CHECK_EQ(2, prologue_call_count);
  CHECK_EQ(2, epilogue_call_count);
  CHECK_EQ(2, prologue_call_count_second);
  CHECK_EQ(2, epilogue_call_count_second);
}


TEST(GCCallbacks) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  gc_callbacks_isolate = isolate;
  isolate->AddGCPrologueCallback(PrologueCallback);
  isolate->AddGCEpilogueCallback(EpilogueCallback);
  CHECK_EQ(0, prologue_call_count);
  CHECK_EQ(0, epilogue_call_count);
  CcTest::heap()->CollectAllGarbage(i::Heap::kNoGCFlags);
  CHECK_EQ(1, prologue_call_count);
  CHECK_EQ(1, epilogue_call_count);
  isolate->AddGCPrologueCallback(PrologueCallbackSecond);
  isolate->AddGCEpilogueCallback(EpilogueCallbackSecond);
  CcTest::heap()->CollectAllGarbage(i::Heap::kNoGCFlags);
  CHECK_EQ(2, prologue_call_count);
  CHECK_EQ(2, epilogue_call_count);
  CHECK_EQ(1, prologue_call_count_second);
  CHECK_EQ(1, epilogue_call_count_second);
  isolate->RemoveGCPrologueCallback(PrologueCallback);
  isolate->RemoveGCEpilogueCallback(EpilogueCallback);
  CcTest::heap()->CollectAllGarbage(i::Heap::kNoGCFlags);
  CHECK_EQ(2, prologue_call_count);
  CHECK_EQ(2, epilogue_call_count);
  CHECK_EQ(2, prologue_call_count_second);
  CHECK_EQ(2, epilogue_call_count_second);
  isolate->RemoveGCPrologueCallback(PrologueCallbackSecond);
  isolate->RemoveGCEpilogueCallback(EpilogueCallbackSecond);
  CcTest::heap()->CollectAllGarbage(i::Heap::kNoGCFlags);
  CHECK_EQ(2, prologue_call_count);
  CHECK_EQ(2, epilogue_call_count);
  CHECK_EQ(2, prologue_call_count_second);
  CHECK_EQ(2, epilogue_call_count_second);

  CHECK_EQ(0, prologue_call_count_alloc);
  CHECK_EQ(0, epilogue_call_count_alloc);
  isolate->AddGCPrologueCallback(PrologueCallbackAlloc);
  isolate->AddGCEpilogueCallback(EpilogueCallbackAlloc);
  CcTest::heap()->CollectAllGarbage(
      i::Heap::kAbortIncrementalMarkingMask);
  CHECK_EQ(1, prologue_call_count_alloc);
  CHECK_EQ(1, epilogue_call_count_alloc);
  isolate->RemoveGCPrologueCallback(PrologueCallbackAlloc);
  isolate->RemoveGCEpilogueCallback(EpilogueCallbackAlloc);
}


THREADED_TEST(AddToJSFunctionResultCache) {
  i::FLAG_stress_compaction = false;
  i::FLAG_allow_natives_syntax = true;
  v8::HandleScope scope(CcTest::isolate());

  LocalContext context;

  const char* code =
      "(function() {"
      "  var key0 = 'a';"
      "  var key1 = 'b';"
      "  var r0 = %_GetFromCache(0, key0);"
      "  var r1 = %_GetFromCache(0, key1);"
      "  var r0_ = %_GetFromCache(0, key0);"
      "  if (r0 !== r0_)"
      "    return 'Different results for ' + key0 + ': ' + r0 + ' vs. ' + r0_;"
      "  var r1_ = %_GetFromCache(0, key1);"
      "  if (r1 !== r1_)"
      "    return 'Different results for ' + key1 + ': ' + r1 + ' vs. ' + r1_;"
      "  return 'PASSED';"
      "})()";
  CcTest::heap()->ClearJSFunctionResultCaches();
  ExpectString(code, "PASSED");
}


THREADED_TEST(FillJSFunctionResultCache) {
  i::FLAG_allow_natives_syntax = true;
  LocalContext context;
  v8::HandleScope scope(context->GetIsolate());

  const char* code =
      "(function() {"
      "  var k = 'a';"
      "  var r = %_GetFromCache(0, k);"
      "  for (var i = 0; i < 16; i++) {"
      "    %_GetFromCache(0, 'a' + i);"
      "  };"
      "  if (r === %_GetFromCache(0, k))"
      "    return 'FAILED: k0CacheSize is too small';"
      "  return 'PASSED';"
      "})()";
  CcTest::heap()->ClearJSFunctionResultCaches();
  ExpectString(code, "PASSED");
}


THREADED_TEST(RoundRobinGetFromCache) {
  i::FLAG_allow_natives_syntax = true;
  LocalContext context;
  v8::HandleScope scope(context->GetIsolate());

  const char* code =
      "(function() {"
      "  var keys = [];"
      "  for (var i = 0; i < 16; i++) keys.push(i);"
      "  var values = [];"
      "  for (var i = 0; i < 16; i++) values[i] = %_GetFromCache(0, keys[i]);"
      "  for (var i = 0; i < 16; i++) {"
      "    var v = %_GetFromCache(0, keys[i]);"
      "    if (v.toString() !== values[i].toString())"
      "      return 'Wrong value for ' + "
      "          keys[i] + ': ' + v + ' vs. ' + values[i];"
      "  };"
      "  return 'PASSED';"
      "})()";
  CcTest::heap()->ClearJSFunctionResultCaches();
  ExpectString(code, "PASSED");
}


THREADED_TEST(ReverseGetFromCache) {
  i::FLAG_allow_natives_syntax = true;
  LocalContext context;
  v8::HandleScope scope(context->GetIsolate());

  const char* code =
      "(function() {"
      "  var keys = [];"
      "  for (var i = 0; i < 16; i++) keys.push(i);"
      "  var values = [];"
      "  for (var i = 0; i < 16; i++) values[i] = %_GetFromCache(0, keys[i]);"
      "  for (var i = 15; i >= 16; i--) {"
      "    var v = %_GetFromCache(0, keys[i]);"
      "    if (v !== values[i])"
      "      return 'Wrong value for ' + "
      "          keys[i] + ': ' + v + ' vs. ' + values[i];"
      "  };"
      "  return 'PASSED';"
      "})()";
  CcTest::heap()->ClearJSFunctionResultCaches();
  ExpectString(code, "PASSED");
}


THREADED_TEST(TestEviction) {
  i::FLAG_allow_natives_syntax = true;
  LocalContext context;
  v8::HandleScope scope(context->GetIsolate());

  const char* code =
      "(function() {"
      "  for (var i = 0; i < 2*16; i++) {"
      "    %_GetFromCache(0, 'a' + i);"
      "  };"
      "  return 'PASSED';"
      "})()";
  CcTest::heap()->ClearJSFunctionResultCaches();
  ExpectString(code, "PASSED");
}


THREADED_TEST(TwoByteStringInOneByteCons) {
  // See Chromium issue 47824.
  LocalContext context;
  v8::HandleScope scope(context->GetIsolate());

  const char* init_code =
      "var str1 = 'abelspendabel';"
      "var str2 = str1 + str1 + str1;"
      "str2;";
  Local<Value> result = CompileRun(init_code);

  Local<Value> indexof = CompileRun("str2.indexOf('els')");
  Local<Value> lastindexof = CompileRun("str2.lastIndexOf('dab')");

  CHECK(result->IsString());
  i::Handle<i::String> string = v8::Utils::OpenHandle(String::Cast(*result));
  int length = string->length();
  CHECK(string->IsOneByteRepresentation());

  i::Handle<i::String> flat_string = i::String::Flatten(string);

  CHECK(string->IsOneByteRepresentation());
  CHECK(flat_string->IsOneByteRepresentation());

  // Create external resource.
  uint16_t* uc16_buffer = new uint16_t[length + 1];

  i::String::WriteToFlat(*flat_string, uc16_buffer, 0, length);
  uc16_buffer[length] = 0;

  TestResource resource(uc16_buffer);

  flat_string->MakeExternal(&resource);

  CHECK(flat_string->IsTwoByteRepresentation());

  // If the cons string has been short-circuited, skip the following checks.
  if (!string.is_identical_to(flat_string)) {
    // At this point, we should have a Cons string which is flat and one-byte,
    // with a first half that is a two-byte string (although it only contains
    // one-byte characters). This is a valid sequence of steps, and it can
    // happen in real pages.
    CHECK(string->IsOneByteRepresentation());
    i::ConsString* cons = i::ConsString::cast(*string);
    CHECK_EQ(0, cons->second()->length());
    CHECK(cons->first()->IsTwoByteRepresentation());
  }

  // Check that some string operations work.

  // Atom RegExp.
  Local<Value> reresult = CompileRun("str2.match(/abel/g).length;");
  CHECK_EQ(6, reresult->Int32Value());

  // Nonatom RegExp.
  reresult = CompileRun("str2.match(/abe./g).length;");
  CHECK_EQ(6, reresult->Int32Value());

  reresult = CompileRun("str2.search(/bel/g);");
  CHECK_EQ(1, reresult->Int32Value());

  reresult = CompileRun("str2.search(/be./g);");
  CHECK_EQ(1, reresult->Int32Value());

  ExpectTrue("/bel/g.test(str2);");

  ExpectTrue("/be./g.test(str2);");

  reresult = CompileRun("/bel/g.exec(str2);");
  CHECK(!reresult->IsNull());

  reresult = CompileRun("/be./g.exec(str2);");
  CHECK(!reresult->IsNull());

  ExpectString("str2.substring(2, 10);", "elspenda");

  ExpectString("str2.substring(2, 20);", "elspendabelabelspe");

  ExpectString("str2.charAt(2);", "e");

  ExpectObject("str2.indexOf('els');", indexof);

  ExpectObject("str2.lastIndexOf('dab');", lastindexof);

  reresult = CompileRun("str2.charCodeAt(2);");
  CHECK_EQ(static_cast<int32_t>('e'), reresult->Int32Value());
}


TEST(ContainsOnlyOneByte) {
  v8::V8::Initialize();
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  // Make a buffer long enough that it won't automatically be converted.
  const int length = 512;
  // Ensure word aligned assignment.
  const int aligned_length = length*sizeof(uintptr_t)/sizeof(uint16_t);
  i::SmartArrayPointer<uintptr_t>
  aligned_contents(new uintptr_t[aligned_length]);
  uint16_t* string_contents =
      reinterpret_cast<uint16_t*>(aligned_contents.get());
  // Set to contain only one byte.
  for (int i = 0; i < length-1; i++) {
    string_contents[i] = 0x41;
  }
  string_contents[length-1] = 0;
  // Simple case.
  Handle<String> string =
      String::NewExternal(isolate,
                          new TestResource(string_contents, NULL, false));
  CHECK(!string->IsOneByte() && string->ContainsOnlyOneByte());
  // Counter example.
  string = String::NewFromTwoByte(isolate, string_contents);
  CHECK(string->IsOneByte() && string->ContainsOnlyOneByte());
  // Test left right and balanced cons strings.
  Handle<String> base = String::NewFromUtf8(isolate, "a");
  Handle<String> left = base;
  Handle<String> right = base;
  for (int i = 0; i < 1000; i++) {
    left = String::Concat(base, left);
    right = String::Concat(right, base);
  }
  Handle<String> balanced = String::Concat(left, base);
  balanced = String::Concat(balanced, right);
  Handle<String> cons_strings[] = {left, balanced, right};
  Handle<String> two_byte =
      String::NewExternal(isolate,
                          new TestResource(string_contents, NULL, false));
  USE(two_byte); USE(cons_strings);
  for (size_t i = 0; i < arraysize(cons_strings); i++) {
    // Base assumptions.
    string = cons_strings[i];
    CHECK(string->IsOneByte() && string->ContainsOnlyOneByte());
    // Test left and right concatentation.
    string = String::Concat(two_byte, cons_strings[i]);
    CHECK(!string->IsOneByte() && string->ContainsOnlyOneByte());
    string = String::Concat(cons_strings[i], two_byte);
    CHECK(!string->IsOneByte() && string->ContainsOnlyOneByte());
  }
  // Set bits in different positions
  // for strings of different lengths and alignments.
  for (int alignment = 0; alignment < 7; alignment++) {
    for (int size = 2; alignment + size < length; size *= 2) {
      int zero_offset = size + alignment;
      string_contents[zero_offset] = 0;
      for (int i = 0; i < size; i++) {
        int shift = 8 + (i % 7);
        string_contents[alignment + i] = 1 << shift;
        string = String::NewExternal(
            isolate,
            new TestResource(string_contents + alignment, NULL, false));
        CHECK_EQ(size, string->Length());
        CHECK(!string->ContainsOnlyOneByte());
        string_contents[alignment + i] = 0x41;
      }
      string_contents[zero_offset] = 0x41;
    }
  }
}


// Failed access check callback that performs a GC on each invocation.
void FailedAccessCheckCallbackGC(Local<v8::Object> target,
                                 v8::AccessType type,
                                 Local<v8::Value> data) {
  CcTest::heap()->CollectAllGarbage(i::Heap::kNoGCFlags);
}


TEST(GCInFailedAccessCheckCallback) {
  // Install a failed access check callback that performs a GC on each
  // invocation. Then force the callback to be called from va

  v8::V8::Initialize();
  v8::V8::SetFailedAccessCheckCallbackFunction(&FailedAccessCheckCallbackGC);

  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);

  // Create an ObjectTemplate for global objects and install access
  // check callbacks that will block access.
  v8::Handle<v8::ObjectTemplate> global_template =
      v8::ObjectTemplate::New(isolate);
  global_template->SetAccessCheckCallbacks(AccessAlwaysBlocked, NULL,
                                           v8::Handle<v8::Value>(), false);

  // Create a context and set an x property on it's global object.
  LocalContext context0(NULL, global_template);
  context0->Global()->Set(v8_str("x"), v8_num(42));
  v8::Handle<v8::Object> global0 = context0->Global();

  // Create a context with a different security token so that the
  // failed access check callback will be called on each access.
  LocalContext context1(NULL, global_template);
  context1->Global()->Set(v8_str("other"), global0);

  // Get property with failed access check.
  ExpectUndefined("other.x");

  // Get element with failed access check.
  ExpectUndefined("other[0]");

  // Set property with failed access check.
  v8::Handle<v8::Value> result = CompileRun("other.x = new Object()");
  CHECK(result->IsObject());

  // Set element with failed access check.
  result = CompileRun("other[0] = new Object()");
  CHECK(result->IsObject());

  // Get property attribute with failed access check.
  ExpectFalse("\'x\' in other");

  // Get property attribute for element with failed access check.
  ExpectFalse("0 in other");

  // Delete property.
  ExpectFalse("delete other.x");

  // Delete element.
  CHECK_EQ(false, global0->Delete(0));

  // DefineAccessor.
  CHECK_EQ(false,
           global0->SetAccessor(v8_str("x"), GetXValue, NULL, v8_str("x")));

  // Define JavaScript accessor.
  ExpectUndefined("Object.prototype.__defineGetter__.call("
                  "    other, \'x\', function() { return 42; })");

  // LookupAccessor.
  ExpectUndefined("Object.prototype.__lookupGetter__.call("
                  "    other, \'x\')");

  // HasOwnElement.
  ExpectFalse("Object.prototype.hasOwnProperty.call(other, \'0\')");

  CHECK_EQ(false, global0->HasRealIndexedProperty(0));
  CHECK_EQ(false, global0->HasRealNamedProperty(v8_str("x")));
  CHECK_EQ(false, global0->HasRealNamedCallbackProperty(v8_str("x")));

  // Reset the failed access check callback so it does not influence
  // the other tests.
  v8::V8::SetFailedAccessCheckCallbackFunction(NULL);
}


TEST(IsolateNewDispose) {
  v8::Isolate* current_isolate = CcTest::isolate();
  v8::Isolate* isolate = v8::Isolate::New();
  CHECK(isolate != NULL);
  CHECK(current_isolate != isolate);
  CHECK(current_isolate == CcTest::isolate());

  v8::V8::SetFatalErrorHandler(StoringErrorCallback);
  last_location = last_message = NULL;
  isolate->Dispose();
  CHECK(!last_location);
  CHECK(!last_message);
}


UNINITIALIZED_TEST(DisposeIsolateWhenInUse) {
  v8::Isolate* isolate = v8::Isolate::New();
  {
    v8::Isolate::Scope i_scope(isolate);
    v8::HandleScope scope(isolate);
    LocalContext context(isolate);
    // Run something in this isolate.
    ExpectTrue("true");
    v8::V8::SetFatalErrorHandler(StoringErrorCallback);
    last_location = last_message = NULL;
    // Still entered, should fail.
    isolate->Dispose();
    CHECK(last_location);
    CHECK(last_message);
  }
  isolate->Dispose();
}


TEST(RunTwoIsolatesOnSingleThread) {
  // Run isolate 1.
  v8::Isolate* isolate1 = v8::Isolate::New();
  isolate1->Enter();
  v8::Persistent<v8::Context> context1;
  {
    v8::HandleScope scope(isolate1);
    context1.Reset(isolate1, Context::New(isolate1));
  }

  {
    v8::HandleScope scope(isolate1);
    v8::Local<v8::Context> context =
        v8::Local<v8::Context>::New(isolate1, context1);
    v8::Context::Scope context_scope(context);
    // Run something in new isolate.
    CompileRun("var foo = 'isolate 1';");
    ExpectString("function f() { return foo; }; f()", "isolate 1");
  }

  // Run isolate 2.
  v8::Isolate* isolate2 = v8::Isolate::New();
  v8::Persistent<v8::Context> context2;

  {
    v8::Isolate::Scope iscope(isolate2);
    v8::HandleScope scope(isolate2);
    context2.Reset(isolate2, Context::New(isolate2));
    v8::Local<v8::Context> context =
        v8::Local<v8::Context>::New(isolate2, context2);
    v8::Context::Scope context_scope(context);

    // Run something in new isolate.
    CompileRun("var foo = 'isolate 2';");
    ExpectString("function f() { return foo; }; f()", "isolate 2");
  }

  {
    v8::HandleScope scope(isolate1);
    v8::Local<v8::Context> context =
        v8::Local<v8::Context>::New(isolate1, context1);
    v8::Context::Scope context_scope(context);
    // Now again in isolate 1
    ExpectString("function f() { return foo; }; f()", "isolate 1");
  }

  isolate1->Exit();

  // Run some stuff in default isolate.
  v8::Persistent<v8::Context> context_default;
  {
    v8::Isolate* isolate = CcTest::isolate();
    v8::Isolate::Scope iscope(isolate);
    v8::HandleScope scope(isolate);
    context_default.Reset(isolate, Context::New(isolate));
  }

  {
    v8::HandleScope scope(CcTest::isolate());
    v8::Local<v8::Context> context =
        v8::Local<v8::Context>::New(CcTest::isolate(), context_default);
    v8::Context::Scope context_scope(context);
    // Variables in other isolates should be not available, verify there
    // is an exception.
    ExpectTrue("function f() {"
               "  try {"
               "    foo;"
               "    return false;"
               "  } catch(e) {"
               "    return true;"
               "  }"
               "};"
               "var isDefaultIsolate = true;"
               "f()");
  }

  isolate1->Enter();

  {
    v8::Isolate::Scope iscope(isolate2);
    v8::HandleScope scope(isolate2);
    v8::Local<v8::Context> context =
        v8::Local<v8::Context>::New(isolate2, context2);
    v8::Context::Scope context_scope(context);
    ExpectString("function f() { return foo; }; f()", "isolate 2");
  }

  {
    v8::HandleScope scope(v8::Isolate::GetCurrent());
    v8::Local<v8::Context> context =
        v8::Local<v8::Context>::New(v8::Isolate::GetCurrent(), context1);
    v8::Context::Scope context_scope(context);
    ExpectString("function f() { return foo; }; f()", "isolate 1");
  }

  {
    v8::Isolate::Scope iscope(isolate2);
    context2.Reset();
  }

  context1.Reset();
  isolate1->Exit();

  v8::V8::SetFatalErrorHandler(StoringErrorCallback);
  last_location = last_message = NULL;

  isolate1->Dispose();
  CHECK(!last_location);
  CHECK(!last_message);

  isolate2->Dispose();
  CHECK(!last_location);
  CHECK(!last_message);

  // Check that default isolate still runs.
  {
    v8::HandleScope scope(CcTest::isolate());
    v8::Local<v8::Context> context =
        v8::Local<v8::Context>::New(CcTest::isolate(), context_default);
    v8::Context::Scope context_scope(context);
    ExpectTrue("function f() { return isDefaultIsolate; }; f()");
  }
}


static int CalcFibonacci(v8::Isolate* isolate, int limit) {
  v8::Isolate::Scope isolate_scope(isolate);
  v8::HandleScope scope(isolate);
  LocalContext context(isolate);
  i::ScopedVector<char> code(1024);
  i::SNPrintF(code, "function fib(n) {"
                    "  if (n <= 2) return 1;"
                    "  return fib(n-1) + fib(n-2);"
                    "}"
                    "fib(%d)", limit);
  Local<Value> value = CompileRun(code.start());
  CHECK(value->IsNumber());
  return static_cast<int>(value->NumberValue());
}

class IsolateThread : public v8::base::Thread {
 public:
  explicit IsolateThread(int fib_limit)
      : Thread(Options("IsolateThread")), fib_limit_(fib_limit), result_(0) {}

  void Run() {
    v8::Isolate* isolate = v8::Isolate::New();
    result_ = CalcFibonacci(isolate, fib_limit_);
    isolate->Dispose();
  }

  int result() { return result_; }

 private:
  int fib_limit_;
  int result_;
};


TEST(MultipleIsolatesOnIndividualThreads) {
  IsolateThread thread1(21);
  IsolateThread thread2(12);

  // Compute some fibonacci numbers on 3 threads in 3 isolates.
  thread1.Start();
  thread2.Start();

  int result1 = CalcFibonacci(CcTest::isolate(), 21);
  int result2 = CalcFibonacci(CcTest::isolate(), 12);

  thread1.Join();
  thread2.Join();

  // Compare results. The actual fibonacci numbers for 12 and 21 are taken
  // (I'm lazy!) from http://en.wikipedia.org/wiki/Fibonacci_number
  CHECK_EQ(result1, 10946);
  CHECK_EQ(result2, 144);
  CHECK_EQ(result1, thread1.result());
  CHECK_EQ(result2, thread2.result());
}


TEST(IsolateDifferentContexts) {
  v8::Isolate* isolate = v8::Isolate::New();
  Local<v8::Context> context;
  {
    v8::Isolate::Scope isolate_scope(isolate);
    v8::HandleScope handle_scope(isolate);
    context = v8::Context::New(isolate);
    v8::Context::Scope context_scope(context);
    Local<Value> v = CompileRun("2");
    CHECK(v->IsNumber());
    CHECK_EQ(2, static_cast<int>(v->NumberValue()));
  }
  {
    v8::Isolate::Scope isolate_scope(isolate);
    v8::HandleScope handle_scope(isolate);
    context = v8::Context::New(isolate);
    v8::Context::Scope context_scope(context);
    Local<Value> v = CompileRun("22");
    CHECK(v->IsNumber());
    CHECK_EQ(22, static_cast<int>(v->NumberValue()));
  }
  isolate->Dispose();
}

class InitDefaultIsolateThread : public v8::base::Thread {
 public:
  enum TestCase {
    SetResourceConstraints,
    SetFatalHandler,
    SetCounterFunction,
    SetCreateHistogramFunction,
    SetAddHistogramSampleFunction
  };

  explicit InitDefaultIsolateThread(TestCase testCase)
      : Thread(Options("InitDefaultIsolateThread")),
        testCase_(testCase),
        result_(false) {}

  void Run() {
    v8::Isolate::CreateParams create_params;
    switch (testCase_) {
      case SetResourceConstraints: {
        create_params.constraints.set_max_semi_space_size(1);
        create_params.constraints.set_max_old_space_size(4);
        break;
      }
      default:
        break;
    }
    v8::Isolate* isolate = v8::Isolate::New(create_params);
    isolate->Enter();
    switch (testCase_) {
      case SetResourceConstraints:
        // Already handled in pre-Isolate-creation block.
        break;

      case SetFatalHandler:
        v8::V8::SetFatalErrorHandler(NULL);
        break;

      case SetCounterFunction:
        CcTest::isolate()->SetCounterFunction(NULL);
        break;

      case SetCreateHistogramFunction:
        CcTest::isolate()->SetCreateHistogramFunction(NULL);
        break;

      case SetAddHistogramSampleFunction:
        CcTest::isolate()->SetAddHistogramSampleFunction(NULL);
        break;
    }
    isolate->Exit();
    isolate->Dispose();
    result_ = true;
  }

  bool result() { return result_; }

 private:
  TestCase testCase_;
  bool result_;
};


static void InitializeTestHelper(InitDefaultIsolateThread::TestCase testCase) {
  InitDefaultIsolateThread thread(testCase);
  thread.Start();
  thread.Join();
  CHECK_EQ(thread.result(), true);
}


TEST(InitializeDefaultIsolateOnSecondaryThread1) {
  InitializeTestHelper(InitDefaultIsolateThread::SetResourceConstraints);
}


TEST(InitializeDefaultIsolateOnSecondaryThread2) {
  InitializeTestHelper(InitDefaultIsolateThread::SetFatalHandler);
}


TEST(InitializeDefaultIsolateOnSecondaryThread3) {
  InitializeTestHelper(InitDefaultIsolateThread::SetCounterFunction);
}


TEST(InitializeDefaultIsolateOnSecondaryThread4) {
  InitializeTestHelper(InitDefaultIsolateThread::SetCreateHistogramFunction);
}


TEST(InitializeDefaultIsolateOnSecondaryThread5) {
  InitializeTestHelper(InitDefaultIsolateThread::SetAddHistogramSampleFunction);
}


TEST(StringCheckMultipleContexts) {
  const char* code =
      "(function() { return \"a\".charAt(0); })()";

  {
    // Run the code twice in the first context to initialize the call IC.
    LocalContext context1;
    v8::HandleScope scope(context1->GetIsolate());
    ExpectString(code, "a");
    ExpectString(code, "a");
  }

  {
    // Change the String.prototype in the second context and check
    // that the right function gets called.
    LocalContext context2;
    v8::HandleScope scope(context2->GetIsolate());
    CompileRun("String.prototype.charAt = function() { return \"not a\"; }");
    ExpectString(code, "not a");
  }
}


TEST(NumberCheckMultipleContexts) {
  const char* code =
      "(function() { return (42).toString(); })()";

  {
    // Run the code twice in the first context to initialize the call IC.
    LocalContext context1;
    v8::HandleScope scope(context1->GetIsolate());
    ExpectString(code, "42");
    ExpectString(code, "42");
  }

  {
    // Change the Number.prototype in the second context and check
    // that the right function gets called.
    LocalContext context2;
    v8::HandleScope scope(context2->GetIsolate());
    CompileRun("Number.prototype.toString = function() { return \"not 42\"; }");
    ExpectString(code, "not 42");
  }
}


TEST(BooleanCheckMultipleContexts) {
  const char* code =
      "(function() { return true.toString(); })()";

  {
    // Run the code twice in the first context to initialize the call IC.
    LocalContext context1;
    v8::HandleScope scope(context1->GetIsolate());
    ExpectString(code, "true");
    ExpectString(code, "true");
  }

  {
    // Change the Boolean.prototype in the second context and check
    // that the right function gets called.
    LocalContext context2;
    v8::HandleScope scope(context2->GetIsolate());
    CompileRun("Boolean.prototype.toString = function() { return \"\"; }");
    ExpectString(code, "");
  }
}


TEST(DontDeleteCellLoadIC) {
  const char* function_code =
      "function readCell() { while (true) { return cell; } }";

  {
    // Run the code twice in the first context to initialize the load
    // IC for a don't delete cell.
    LocalContext context1;
    v8::HandleScope scope(context1->GetIsolate());
    CompileRun("var cell = \"first\";");
    ExpectBoolean("delete cell", false);
    CompileRun(function_code);
    ExpectString("readCell()", "first");
    ExpectString("readCell()", "first");
  }

  {
    // Use a deletable cell in the second context.
    LocalContext context2;
    v8::HandleScope scope(context2->GetIsolate());
    CompileRun("cell = \"second\";");
    CompileRun(function_code);
    ExpectString("readCell()", "second");
    ExpectBoolean("delete cell", true);
    ExpectString("(function() {"
                 "  try {"
                 "    return readCell();"
                 "  } catch(e) {"
                 "    return e.toString();"
                 "  }"
                 "})()",
                 "ReferenceError: cell is not defined");
    CompileRun("cell = \"new_second\";");
    CcTest::heap()->CollectAllGarbage(i::Heap::kNoGCFlags);
    ExpectString("readCell()", "new_second");
    ExpectString("readCell()", "new_second");
  }
}


class Visitor42 : public v8::PersistentHandleVisitor {
 public:
  explicit Visitor42(v8::Persistent<v8::Object>* object)
      : counter_(0), object_(object) { }

  virtual void VisitPersistentHandle(Persistent<Value>* value,
                                     uint16_t class_id) {
    if (class_id != 42) return;
    CHECK_EQ(42, value->WrapperClassId());
    v8::Isolate* isolate = CcTest::isolate();
    v8::HandleScope handle_scope(isolate);
    v8::Handle<v8::Value> handle = v8::Local<v8::Value>::New(isolate, *value);
    v8::Handle<v8::Value> object =
        v8::Local<v8::Object>::New(isolate, *object_);
    CHECK(handle->IsObject());
    CHECK(Handle<Object>::Cast(handle)->Equals(object));
    ++counter_;
  }

  int counter_;
  v8::Persistent<v8::Object>* object_;
};


TEST(PersistentHandleVisitor) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);
  v8::Persistent<v8::Object> object(isolate, v8::Object::New(isolate));
  CHECK_EQ(0, object.WrapperClassId());
  object.SetWrapperClassId(42);
  CHECK_EQ(42, object.WrapperClassId());

  Visitor42 visitor(&object);
  v8::V8::VisitHandlesWithClassIds(isolate, &visitor);
  CHECK_EQ(1, visitor.counter_);

  object.Reset();
}


TEST(WrapperClassId) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);
  v8::Persistent<v8::Object> object(isolate, v8::Object::New(isolate));
  CHECK_EQ(0, object.WrapperClassId());
  object.SetWrapperClassId(65535);
  CHECK_EQ(65535, object.WrapperClassId());
  object.Reset();
}


TEST(PersistentHandleInNewSpaceVisitor) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);
  v8::Persistent<v8::Object> object1(isolate, v8::Object::New(isolate));
  CHECK_EQ(0, object1.WrapperClassId());
  object1.SetWrapperClassId(42);
  CHECK_EQ(42, object1.WrapperClassId());

  CcTest::heap()->CollectAllGarbage(i::Heap::kNoGCFlags);
  CcTest::heap()->CollectAllGarbage(i::Heap::kNoGCFlags);

  v8::Persistent<v8::Object> object2(isolate, v8::Object::New(isolate));
  CHECK_EQ(0, object2.WrapperClassId());
  object2.SetWrapperClassId(42);
  CHECK_EQ(42, object2.WrapperClassId());

  Visitor42 visitor(&object2);
  v8::V8::VisitHandlesForPartialDependence(isolate, &visitor);
  CHECK_EQ(1, visitor.counter_);

  object1.Reset();
  object2.Reset();
}


TEST(RegExp) {
  LocalContext context;
  v8::HandleScope scope(context->GetIsolate());

  v8::Handle<v8::RegExp> re = v8::RegExp::New(v8_str("foo"), v8::RegExp::kNone);
  CHECK(re->IsRegExp());
  CHECK(re->GetSource()->Equals(v8_str("foo")));
  CHECK_EQ(v8::RegExp::kNone, re->GetFlags());

  re = v8::RegExp::New(v8_str("bar"),
                       static_cast<v8::RegExp::Flags>(v8::RegExp::kIgnoreCase |
                                                      v8::RegExp::kGlobal));
  CHECK(re->IsRegExp());
  CHECK(re->GetSource()->Equals(v8_str("bar")));
  CHECK_EQ(v8::RegExp::kIgnoreCase | v8::RegExp::kGlobal,
           static_cast<int>(re->GetFlags()));

  re = v8::RegExp::New(v8_str("baz"),
                       static_cast<v8::RegExp::Flags>(v8::RegExp::kIgnoreCase |
                                                      v8::RegExp::kMultiline));
  CHECK(re->IsRegExp());
  CHECK(re->GetSource()->Equals(v8_str("baz")));
  CHECK_EQ(v8::RegExp::kIgnoreCase | v8::RegExp::kMultiline,
           static_cast<int>(re->GetFlags()));

  re = CompileRun("/quux/").As<v8::RegExp>();
  CHECK(re->IsRegExp());
  CHECK(re->GetSource()->Equals(v8_str("quux")));
  CHECK_EQ(v8::RegExp::kNone, re->GetFlags());

  re = CompileRun("/quux/gm").As<v8::RegExp>();
  CHECK(re->IsRegExp());
  CHECK(re->GetSource()->Equals(v8_str("quux")));
  CHECK_EQ(v8::RegExp::kGlobal | v8::RegExp::kMultiline,
           static_cast<int>(re->GetFlags()));

  // Override the RegExp constructor and check the API constructor
  // still works.
  CompileRun("RegExp = function() {}");

  re = v8::RegExp::New(v8_str("foobar"), v8::RegExp::kNone);
  CHECK(re->IsRegExp());
  CHECK(re->GetSource()->Equals(v8_str("foobar")));
  CHECK_EQ(v8::RegExp::kNone, re->GetFlags());

  re = v8::RegExp::New(v8_str("foobarbaz"),
                       static_cast<v8::RegExp::Flags>(v8::RegExp::kIgnoreCase |
                                                      v8::RegExp::kMultiline));
  CHECK(re->IsRegExp());
  CHECK(re->GetSource()->Equals(v8_str("foobarbaz")));
  CHECK_EQ(v8::RegExp::kIgnoreCase | v8::RegExp::kMultiline,
           static_cast<int>(re->GetFlags()));

  context->Global()->Set(v8_str("re"), re);
  ExpectTrue("re.test('FoobarbaZ')");

  // RegExps are objects on which you can set properties.
  re->Set(v8_str("property"), v8::Integer::New(context->GetIsolate(), 32));
  v8::Handle<v8::Value> value(CompileRun("re.property"));
  CHECK_EQ(32, value->Int32Value());

  v8::TryCatch try_catch;
  re = v8::RegExp::New(v8_str("foo["), v8::RegExp::kNone);
  CHECK(re.IsEmpty());
  CHECK(try_catch.HasCaught());
  context->Global()->Set(v8_str("ex"), try_catch.Exception());
  ExpectTrue("ex instanceof SyntaxError");
}


THREADED_TEST(Equals) {
  LocalContext localContext;
  v8::HandleScope handleScope(localContext->GetIsolate());

  v8::Handle<v8::Object> globalProxy = localContext->Global();
  v8::Handle<Value> global = globalProxy->GetPrototype();

  CHECK(global->StrictEquals(global));
  CHECK(!global->StrictEquals(globalProxy));
  CHECK(!globalProxy->StrictEquals(global));
  CHECK(globalProxy->StrictEquals(globalProxy));

  CHECK(global->Equals(global));
  CHECK(!global->Equals(globalProxy));
  CHECK(!globalProxy->Equals(global));
  CHECK(globalProxy->Equals(globalProxy));
}


static void Getter(v8::Local<v8::Name> property,
                   const v8::PropertyCallbackInfo<v8::Value>& info) {
  info.GetReturnValue().Set(v8_str("42!"));
}


static void Enumerator(const v8::PropertyCallbackInfo<v8::Array>& info) {
  v8::Handle<v8::Array> result = v8::Array::New(info.GetIsolate());
  result->Set(0, v8_str("universalAnswer"));
  info.GetReturnValue().Set(result);
}


TEST(NamedEnumeratorAndForIn) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope handle_scope(isolate);
  v8::Context::Scope context_scope(context.local());

  v8::Handle<v8::ObjectTemplate> tmpl = v8::ObjectTemplate::New(isolate);
  tmpl->SetHandler(v8::NamedPropertyHandlerConfiguration(Getter, NULL, NULL,
                                                         NULL, Enumerator));
  context->Global()->Set(v8_str("o"), tmpl->NewInstance());
  v8::Handle<v8::Array> result = v8::Handle<v8::Array>::Cast(CompileRun(
        "var result = []; for (var k in o) result.push(k); result"));
  CHECK_EQ(1u, result->Length());
  CHECK(v8_str("universalAnswer")->Equals(result->Get(0)));
}


TEST(DefinePropertyPostDetach) {
  LocalContext context;
  v8::HandleScope scope(context->GetIsolate());
  v8::Handle<v8::Object> proxy = context->Global();
  v8::Handle<v8::Function> define_property =
      CompileRun("(function() {"
                 "  Object.defineProperty("
                 "    this,"
                 "    1,"
                 "    { configurable: true, enumerable: true, value: 3 });"
                 "})").As<Function>();
  context->DetachGlobal();
  define_property->Call(proxy, 0, NULL);
}


static void InstallContextId(v8::Handle<Context> context, int id) {
  Context::Scope scope(context);
  CompileRun("Object.prototype").As<Object>()->
      Set(v8_str("context_id"), v8::Integer::New(context->GetIsolate(), id));
}


static void CheckContextId(v8::Handle<Object> object, int expected) {
  CHECK_EQ(expected, object->Get(v8_str("context_id"))->Int32Value());
}


THREADED_TEST(CreationContext) {
  v8::Isolate* isolate = CcTest::isolate();
  HandleScope handle_scope(isolate);
  Handle<Context> context1 = Context::New(isolate);
  InstallContextId(context1, 1);
  Handle<Context> context2 = Context::New(isolate);
  InstallContextId(context2, 2);
  Handle<Context> context3 = Context::New(isolate);
  InstallContextId(context3, 3);

  Local<v8::FunctionTemplate> tmpl = v8::FunctionTemplate::New(isolate);

  Local<Object> object1;
  Local<Function> func1;
  {
    Context::Scope scope(context1);
    object1 = Object::New(isolate);
    func1 = tmpl->GetFunction();
  }

  Local<Object> object2;
  Local<Function> func2;
  {
    Context::Scope scope(context2);
    object2 = Object::New(isolate);
    func2 = tmpl->GetFunction();
  }

  Local<Object> instance1;
  Local<Object> instance2;

  {
    Context::Scope scope(context3);
    instance1 = func1->NewInstance();
    instance2 = func2->NewInstance();
  }

  {
    Handle<Context> other_context = Context::New(isolate);
    Context::Scope scope(other_context);
    CHECK(object1->CreationContext() == context1);
    CheckContextId(object1, 1);
    CHECK(func1->CreationContext() == context1);
    CheckContextId(func1, 1);
    CHECK(instance1->CreationContext() == context1);
    CheckContextId(instance1, 1);
    CHECK(object2->CreationContext() == context2);
    CheckContextId(object2, 2);
    CHECK(func2->CreationContext() == context2);
    CheckContextId(func2, 2);
    CHECK(instance2->CreationContext() == context2);
    CheckContextId(instance2, 2);
  }

  {
    Context::Scope scope(context1);
    CHECK(object1->CreationContext() == context1);
    CheckContextId(object1, 1);
    CHECK(func1->CreationContext() == context1);
    CheckContextId(func1, 1);
    CHECK(instance1->CreationContext() == context1);
    CheckContextId(instance1, 1);
    CHECK(object2->CreationContext() == context2);
    CheckContextId(object2, 2);
    CHECK(func2->CreationContext() == context2);
    CheckContextId(func2, 2);
    CHECK(instance2->CreationContext() == context2);
    CheckContextId(instance2, 2);
  }

  {
    Context::Scope scope(context2);
    CHECK(object1->CreationContext() == context1);
    CheckContextId(object1, 1);
    CHECK(func1->CreationContext() == context1);
    CheckContextId(func1, 1);
    CHECK(instance1->CreationContext() == context1);
    CheckContextId(instance1, 1);
    CHECK(object2->CreationContext() == context2);
    CheckContextId(object2, 2);
    CHECK(func2->CreationContext() == context2);
    CheckContextId(func2, 2);
    CHECK(instance2->CreationContext() == context2);
    CheckContextId(instance2, 2);
  }
}


THREADED_TEST(CreationContextOfJsFunction) {
  HandleScope handle_scope(CcTest::isolate());
  Handle<Context> context = Context::New(CcTest::isolate());
  InstallContextId(context, 1);

  Local<Object> function;
  {
    Context::Scope scope(context);
    function = CompileRun("function foo() {}; foo").As<Object>();
  }

  Handle<Context> other_context = Context::New(CcTest::isolate());
  Context::Scope scope(other_context);
  CHECK(function->CreationContext() == context);
  CheckContextId(function, 1);
}


void HasOwnPropertyIndexedPropertyGetter(
    uint32_t index,
    const v8::PropertyCallbackInfo<v8::Value>& info) {
  if (index == 42) info.GetReturnValue().Set(v8_str("yes"));
}


void HasOwnPropertyNamedPropertyGetter(
    Local<Name> property, const v8::PropertyCallbackInfo<v8::Value>& info) {
  if (property->Equals(v8_str("foo"))) info.GetReturnValue().Set(v8_str("yes"));
}


void HasOwnPropertyIndexedPropertyQuery(
    uint32_t index, const v8::PropertyCallbackInfo<v8::Integer>& info) {
  if (index == 42) info.GetReturnValue().Set(1);
}


void HasOwnPropertyNamedPropertyQuery(
    Local<Name> property, const v8::PropertyCallbackInfo<v8::Integer>& info) {
  if (property->Equals(v8_str("foo"))) info.GetReturnValue().Set(1);
}


void HasOwnPropertyNamedPropertyQuery2(
    Local<Name> property, const v8::PropertyCallbackInfo<v8::Integer>& info) {
  if (property->Equals(v8_str("bar"))) info.GetReturnValue().Set(1);
}


void HasOwnPropertyAccessorGetter(
    Local<String> property,
    const v8::PropertyCallbackInfo<v8::Value>& info) {
  info.GetReturnValue().Set(v8_str("yes"));
}


TEST(HasOwnProperty) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);
  { // Check normal properties and defined getters.
    Handle<Value> value = CompileRun(
        "function Foo() {"
        "    this.foo = 11;"
        "    this.__defineGetter__('baz', function() { return 1; });"
        "};"
        "function Bar() { "
        "    this.bar = 13;"
        "    this.__defineGetter__('bla', function() { return 2; });"
        "};"
        "Bar.prototype = new Foo();"
        "new Bar();");
    CHECK(value->IsObject());
    Handle<Object> object = value->ToObject(isolate);
    CHECK(object->Has(v8_str("foo")));
    CHECK(!object->HasOwnProperty(v8_str("foo")));
    CHECK(object->HasOwnProperty(v8_str("bar")));
    CHECK(object->Has(v8_str("baz")));
    CHECK(!object->HasOwnProperty(v8_str("baz")));
    CHECK(object->HasOwnProperty(v8_str("bla")));
  }
  { // Check named getter interceptors.
    Handle<ObjectTemplate> templ = ObjectTemplate::New(isolate);
    templ->SetHandler(v8::NamedPropertyHandlerConfiguration(
        HasOwnPropertyNamedPropertyGetter));
    Handle<Object> instance = templ->NewInstance();
    CHECK(!instance->HasOwnProperty(v8_str("42")));
    CHECK(instance->HasOwnProperty(v8_str("foo")));
    CHECK(!instance->HasOwnProperty(v8_str("bar")));
  }
  { // Check indexed getter interceptors.
    Handle<ObjectTemplate> templ = ObjectTemplate::New(isolate);
    templ->SetHandler(v8::IndexedPropertyHandlerConfiguration(
        HasOwnPropertyIndexedPropertyGetter));
    Handle<Object> instance = templ->NewInstance();
    CHECK(instance->HasOwnProperty(v8_str("42")));
    CHECK(!instance->HasOwnProperty(v8_str("43")));
    CHECK(!instance->HasOwnProperty(v8_str("foo")));
  }
  { // Check named query interceptors.
    Handle<ObjectTemplate> templ = ObjectTemplate::New(isolate);
    templ->SetHandler(v8::NamedPropertyHandlerConfiguration(
        0, 0, HasOwnPropertyNamedPropertyQuery));
    Handle<Object> instance = templ->NewInstance();
    CHECK(instance->HasOwnProperty(v8_str("foo")));
    CHECK(!instance->HasOwnProperty(v8_str("bar")));
  }
  { // Check indexed query interceptors.
    Handle<ObjectTemplate> templ = ObjectTemplate::New(isolate);
    templ->SetHandler(v8::IndexedPropertyHandlerConfiguration(
        0, 0, HasOwnPropertyIndexedPropertyQuery));
    Handle<Object> instance = templ->NewInstance();
    CHECK(instance->HasOwnProperty(v8_str("42")));
    CHECK(!instance->HasOwnProperty(v8_str("41")));
  }
  { // Check callbacks.
    Handle<ObjectTemplate> templ = ObjectTemplate::New(isolate);
    templ->SetAccessor(v8_str("foo"), HasOwnPropertyAccessorGetter);
    Handle<Object> instance = templ->NewInstance();
    CHECK(instance->HasOwnProperty(v8_str("foo")));
    CHECK(!instance->HasOwnProperty(v8_str("bar")));
  }
  { // Check that query wins on disagreement.
    Handle<ObjectTemplate> templ = ObjectTemplate::New(isolate);
    templ->SetHandler(v8::NamedPropertyHandlerConfiguration(
        HasOwnPropertyNamedPropertyGetter, 0,
        HasOwnPropertyNamedPropertyQuery2));
    Handle<Object> instance = templ->NewInstance();
    CHECK(!instance->HasOwnProperty(v8_str("foo")));
    CHECK(instance->HasOwnProperty(v8_str("bar")));
  }
}


TEST(IndexedInterceptorWithStringProto) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  Handle<ObjectTemplate> templ = ObjectTemplate::New(isolate);
  templ->SetHandler(v8::IndexedPropertyHandlerConfiguration(
      NULL, NULL, HasOwnPropertyIndexedPropertyQuery));
  LocalContext context;
  context->Global()->Set(v8_str("obj"), templ->NewInstance());
  CompileRun("var s = new String('foobar'); obj.__proto__ = s;");
  // These should be intercepted.
  CHECK(CompileRun("42 in obj")->BooleanValue());
  CHECK(CompileRun("'42' in obj")->BooleanValue());
  // These should fall through to the String prototype.
  CHECK(CompileRun("0 in obj")->BooleanValue());
  CHECK(CompileRun("'0' in obj")->BooleanValue());
  // And these should both fail.
  CHECK(!CompileRun("32 in obj")->BooleanValue());
  CHECK(!CompileRun("'32' in obj")->BooleanValue());
}


void CheckCodeGenerationAllowed() {
  Handle<Value> result = CompileRun("eval('42')");
  CHECK_EQ(42, result->Int32Value());
  result = CompileRun("(function(e) { return e('42'); })(eval)");
  CHECK_EQ(42, result->Int32Value());
  result = CompileRun("var f = new Function('return 42'); f()");
  CHECK_EQ(42, result->Int32Value());
}


void CheckCodeGenerationDisallowed() {
  TryCatch try_catch;

  Handle<Value> result = CompileRun("eval('42')");
  CHECK(result.IsEmpty());
  CHECK(try_catch.HasCaught());
  try_catch.Reset();

  result = CompileRun("(function(e) { return e('42'); })(eval)");
  CHECK(result.IsEmpty());
  CHECK(try_catch.HasCaught());
  try_catch.Reset();

  result = CompileRun("var f = new Function('return 42'); f()");
  CHECK(result.IsEmpty());
  CHECK(try_catch.HasCaught());
}


bool CodeGenerationAllowed(Local<Context> context) {
  ApiTestFuzzer::Fuzz();
  return true;
}


bool CodeGenerationDisallowed(Local<Context> context) {
  ApiTestFuzzer::Fuzz();
  return false;
}


THREADED_TEST(AllowCodeGenFromStrings) {
  LocalContext context;
  v8::HandleScope scope(context->GetIsolate());

  // eval and the Function constructor allowed by default.
  CHECK(context->IsCodeGenerationFromStringsAllowed());
  CheckCodeGenerationAllowed();

  // Disallow eval and the Function constructor.
  context->AllowCodeGenerationFromStrings(false);
  CHECK(!context->IsCodeGenerationFromStringsAllowed());
  CheckCodeGenerationDisallowed();

  // Allow again.
  context->AllowCodeGenerationFromStrings(true);
  CheckCodeGenerationAllowed();

  // Disallow but setting a global callback that will allow the calls.
  context->AllowCodeGenerationFromStrings(false);
  V8::SetAllowCodeGenerationFromStringsCallback(&CodeGenerationAllowed);
  CHECK(!context->IsCodeGenerationFromStringsAllowed());
  CheckCodeGenerationAllowed();

  // Set a callback that disallows the code generation.
  V8::SetAllowCodeGenerationFromStringsCallback(&CodeGenerationDisallowed);
  CHECK(!context->IsCodeGenerationFromStringsAllowed());
  CheckCodeGenerationDisallowed();
}


TEST(SetErrorMessageForCodeGenFromStrings) {
  LocalContext context;
  v8::HandleScope scope(context->GetIsolate());
  TryCatch try_catch;

  Handle<String> message = v8_str("Message") ;
  Handle<String> expected_message = v8_str("Uncaught EvalError: Message");
  V8::SetAllowCodeGenerationFromStringsCallback(&CodeGenerationDisallowed);
  context->AllowCodeGenerationFromStrings(false);
  context->SetErrorMessageForCodeGenerationFromStrings(message);
  Handle<Value> result = CompileRun("eval('42')");
  CHECK(result.IsEmpty());
  CHECK(try_catch.HasCaught());
  Handle<String> actual_message = try_catch.Message()->Get();
  CHECK(expected_message->Equals(actual_message));
}


static void NonObjectThis(const v8::FunctionCallbackInfo<v8::Value>& args) {
}


THREADED_TEST(CallAPIFunctionOnNonObject) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);
  Handle<FunctionTemplate> templ =
      v8::FunctionTemplate::New(isolate, NonObjectThis);
  Handle<Function> function = templ->GetFunction();
  context->Global()->Set(v8_str("f"), function);
  TryCatch try_catch;
  CompileRun("f.call(2)");
}


// Regression test for issue 1470.
THREADED_TEST(ReadOnlyIndexedProperties) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  Local<ObjectTemplate> templ = ObjectTemplate::New(isolate);

  LocalContext context;
  Local<v8::Object> obj = templ->NewInstance();
  context->Global()->Set(v8_str("obj"), obj);
  obj->ForceSet(v8_str("1"), v8_str("DONT_CHANGE"), v8::ReadOnly);
  obj->Set(v8_str("1"), v8_str("foobar"));
  CHECK(v8_str("DONT_CHANGE")->Equals(obj->Get(v8_str("1"))));
  obj->ForceSet(v8_num(2), v8_str("DONT_CHANGE"), v8::ReadOnly);
  obj->Set(v8_num(2), v8_str("foobar"));
  CHECK(v8_str("DONT_CHANGE")->Equals(obj->Get(v8_num(2))));

  // Test non-smi case.
  obj->ForceSet(v8_str("2000000000"), v8_str("DONT_CHANGE"), v8::ReadOnly);
  obj->Set(v8_str("2000000000"), v8_str("foobar"));
  CHECK(v8_str("DONT_CHANGE")->Equals(obj->Get(v8_str("2000000000"))));
}


static int CountLiveMapsInMapCache(i::Context* context) {
  i::FixedArray* map_cache = i::FixedArray::cast(context->map_cache());
  int length = map_cache->length();
  int count = 0;
  for (int i = 0; i < length; i++) {
    i::Object* value = map_cache->get(i);
    if (value->IsWeakCell() && !i::WeakCell::cast(value)->cleared()) count++;
  }
  return count;
}


THREADED_TEST(Regress1516) {
  LocalContext context;
  v8::HandleScope scope(context->GetIsolate());

  // Object with 20 properties is not a common case, so it should be removed
  // from the cache after GC.
  { v8::HandleScope temp_scope(context->GetIsolate());
    CompileRun(
        "({"
        "'a00': 0, 'a01': 0, 'a02': 0, 'a03': 0, 'a04': 0, "
        "'a05': 0, 'a06': 0, 'a07': 0, 'a08': 0, 'a09': 0, "
        "'a10': 0, 'a11': 0, 'a12': 0, 'a13': 0, 'a14': 0, "
        "'a15': 0, 'a16': 0, 'a17': 0, 'a18': 0, 'a19': 0, "
        "})");
  }

  int elements = CountLiveMapsInMapCache(CcTest::i_isolate()->context());
  CHECK_LE(1, elements);

  CcTest::heap()->CollectAllGarbage(i::Heap::kAbortIncrementalMarkingMask);

  CHECK_GT(elements, CountLiveMapsInMapCache(CcTest::i_isolate()->context()));
}


THREADED_TEST(Regress93759) {
  v8::Isolate* isolate = CcTest::isolate();
  HandleScope scope(isolate);

  // Template for object with security check.
  Local<ObjectTemplate> no_proto_template = v8::ObjectTemplate::New(isolate);
  no_proto_template->SetAccessCheckCallbacks(AccessAlwaysBlocked, NULL);

  // Templates for objects with hidden prototypes and possibly security check.
  Local<FunctionTemplate> hidden_proto_template =
      v8::FunctionTemplate::New(isolate);
  hidden_proto_template->SetHiddenPrototype(true);

  Local<FunctionTemplate> protected_hidden_proto_template =
      v8::FunctionTemplate::New(isolate);
  protected_hidden_proto_template->InstanceTemplate()->SetAccessCheckCallbacks(
      AccessAlwaysBlocked, NULL);
  protected_hidden_proto_template->SetHiddenPrototype(true);

  // Context for "foreign" objects used in test.
  Local<Context> context = v8::Context::New(isolate);
  context->Enter();

  // Plain object, no security check.
  Local<Object> simple_object = Object::New(isolate);

  // Object with explicit security check.
  Local<Object> protected_object = no_proto_template->NewInstance();

  // JSGlobalProxy object, always have security check.
  Local<Object> proxy_object = context->Global();

  // Global object, the  prototype of proxy_object. No security checks.
  Local<Object> global_object = proxy_object->GetPrototype()->ToObject(isolate);

  // Hidden prototype without security check.
  Local<Object> hidden_prototype =
      hidden_proto_template->GetFunction()->NewInstance();
  Local<Object> object_with_hidden =
    Object::New(isolate);
  object_with_hidden->SetPrototype(hidden_prototype);

  // Hidden prototype with security check on the hidden prototype.
  Local<Object> protected_hidden_prototype =
      protected_hidden_proto_template->GetFunction()->NewInstance();
  Local<Object> object_with_protected_hidden =
    Object::New(isolate);
  object_with_protected_hidden->SetPrototype(protected_hidden_prototype);

  context->Exit();

  // Template for object for second context. Values to test are put on it as
  // properties.
  Local<ObjectTemplate> global_template = ObjectTemplate::New(isolate);
  global_template->Set(v8_str("simple"), simple_object);
  global_template->Set(v8_str("protected"), protected_object);
  global_template->Set(v8_str("global"), global_object);
  global_template->Set(v8_str("proxy"), proxy_object);
  global_template->Set(v8_str("hidden"), object_with_hidden);
  global_template->Set(v8_str("phidden"), object_with_protected_hidden);

  LocalContext context2(NULL, global_template);

  Local<Value> result1 = CompileRun("Object.getPrototypeOf(simple)");
  CHECK(result1->Equals(simple_object->GetPrototype()));

  Local<Value> result2 = CompileRun("Object.getPrototypeOf(protected)");
  CHECK(result2.IsEmpty());

  Local<Value> result3 = CompileRun("Object.getPrototypeOf(global)");
  CHECK(result3->Equals(global_object->GetPrototype()));

  Local<Value> result4 = CompileRun("Object.getPrototypeOf(proxy)");
  CHECK(result4.IsEmpty());

  Local<Value> result5 = CompileRun("Object.getPrototypeOf(hidden)");
  CHECK(result5->Equals(
      object_with_hidden->GetPrototype()->ToObject(isolate)->GetPrototype()));

  Local<Value> result6 = CompileRun("Object.getPrototypeOf(phidden)");
  CHECK(result6.IsEmpty());
}


static void TestReceiver(Local<Value> expected_result,
                         Local<Value> expected_receiver,
                         const char* code) {
  Local<Value> result = CompileRun(code);
  CHECK(result->IsObject());
  CHECK(expected_receiver->Equals(result.As<v8::Object>()->Get(1)));
  CHECK(expected_result->Equals(result.As<v8::Object>()->Get(0)));
}


THREADED_TEST(ForeignFunctionReceiver) {
  v8::Isolate* isolate = CcTest::isolate();
  HandleScope scope(isolate);

  // Create two contexts with different "id" properties ('i' and 'o').
  // Call a function both from its own context and from a the foreign
  // context, and see what "this" is bound to (returning both "this"
  // and "this.id" for comparison).

  Local<Context> foreign_context = v8::Context::New(isolate);
  foreign_context->Enter();
  Local<Value> foreign_function =
    CompileRun("function func() { return { 0: this.id, "
               "                           1: this, "
               "                           toString: function() { "
               "                               return this[0];"
               "                           }"
               "                         };"
               "}"
               "var id = 'i';"
               "func;");
  CHECK(foreign_function->IsFunction());
  foreign_context->Exit();

  LocalContext context;

  Local<String> password = v8_str("Password");
  // Don't get hit by security checks when accessing foreign_context's
  // global receiver (aka. global proxy).
  context->SetSecurityToken(password);
  foreign_context->SetSecurityToken(password);

  Local<String> i = v8_str("i");
  Local<String> o = v8_str("o");
  Local<String> id = v8_str("id");

  CompileRun("function ownfunc() { return { 0: this.id, "
             "                              1: this, "
             "                              toString: function() { "
             "                                  return this[0];"
             "                              }"
             "                             };"
             "}"
             "var id = 'o';"
             "ownfunc");
  context->Global()->Set(v8_str("func"), foreign_function);

  // Sanity check the contexts.
  CHECK(i->Equals(foreign_context->Global()->Get(id)));
  CHECK(o->Equals(context->Global()->Get(id)));

  // Checking local function's receiver.
  // Calling function using its call/apply methods.
  TestReceiver(o, context->Global(), "ownfunc.call()");
  TestReceiver(o, context->Global(), "ownfunc.apply()");
  // Making calls through built-in functions.
  TestReceiver(o, context->Global(), "[1].map(ownfunc)[0]");
  CHECK(o->Equals(CompileRun("'abcbd'.replace(/b/,ownfunc)[1]")));
  CHECK(o->Equals(CompileRun("'abcbd'.replace(/b/g,ownfunc)[1]")));
  CHECK(o->Equals(CompileRun("'abcbd'.replace(/b/g,ownfunc)[3]")));
  // Calling with environment record as base.
  TestReceiver(o, context->Global(), "ownfunc()");
  // Calling with no base.
  TestReceiver(o, context->Global(), "(1,ownfunc)()");

  // Checking foreign function return value.
  // Calling function using its call/apply methods.
  TestReceiver(i, foreign_context->Global(), "func.call()");
  TestReceiver(i, foreign_context->Global(), "func.apply()");
  // Calling function using another context's call/apply methods.
  TestReceiver(i, foreign_context->Global(),
               "Function.prototype.call.call(func)");
  TestReceiver(i, foreign_context->Global(),
               "Function.prototype.call.apply(func)");
  TestReceiver(i, foreign_context->Global(),
               "Function.prototype.apply.call(func)");
  TestReceiver(i, foreign_context->Global(),
               "Function.prototype.apply.apply(func)");
  // Making calls through built-in functions.
  TestReceiver(i, foreign_context->Global(), "[1].map(func)[0]");
  // ToString(func()) is func()[0], i.e., the returned this.id.
  CHECK(i->Equals(CompileRun("'abcbd'.replace(/b/,func)[1]")));
  CHECK(i->Equals(CompileRun("'abcbd'.replace(/b/g,func)[1]")));
  CHECK(i->Equals(CompileRun("'abcbd'.replace(/b/g,func)[3]")));

  // Calling with environment record as base.
  TestReceiver(i, foreign_context->Global(), "func()");
  // Calling with no base.
  TestReceiver(i, foreign_context->Global(), "(1,func)()");
}


uint8_t callback_fired = 0;


void CallCompletedCallback1() {
  v8::base::OS::Print("Firing callback 1.\n");
  callback_fired ^= 1;  // Toggle first bit.
}


void CallCompletedCallback2() {
  v8::base::OS::Print("Firing callback 2.\n");
  callback_fired ^= 2;  // Toggle second bit.
}


void RecursiveCall(const v8::FunctionCallbackInfo<v8::Value>& args) {
  int32_t level = args[0]->Int32Value();
  if (level < 3) {
    level++;
    v8::base::OS::Print("Entering recursion level %d.\n", level);
    char script[64];
    i::Vector<char> script_vector(script, sizeof(script));
    i::SNPrintF(script_vector, "recursion(%d)", level);
    CompileRun(script_vector.start());
    v8::base::OS::Print("Leaving recursion level %d.\n", level);
    CHECK_EQ(0, callback_fired);
  } else {
    v8::base::OS::Print("Recursion ends.\n");
    CHECK_EQ(0, callback_fired);
  }
}


TEST(CallCompletedCallback) {
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());
  v8::Handle<v8::FunctionTemplate> recursive_runtime =
      v8::FunctionTemplate::New(env->GetIsolate(), RecursiveCall);
  env->Global()->Set(v8_str("recursion"),
                     recursive_runtime->GetFunction());
  // Adding the same callback a second time has no effect.
  env->GetIsolate()->AddCallCompletedCallback(CallCompletedCallback1);
  env->GetIsolate()->AddCallCompletedCallback(CallCompletedCallback1);
  env->GetIsolate()->AddCallCompletedCallback(CallCompletedCallback2);
  v8::base::OS::Print("--- Script (1) ---\n");
  Local<Script> script = v8::Script::Compile(
      v8::String::NewFromUtf8(env->GetIsolate(), "recursion(0)"));
  script->Run();
  CHECK_EQ(3, callback_fired);

  v8::base::OS::Print("\n--- Script (2) ---\n");
  callback_fired = 0;
  env->GetIsolate()->RemoveCallCompletedCallback(CallCompletedCallback1);
  script->Run();
  CHECK_EQ(2, callback_fired);

  v8::base::OS::Print("\n--- Function ---\n");
  callback_fired = 0;
  Local<Function> recursive_function =
      Local<Function>::Cast(env->Global()->Get(v8_str("recursion")));
  v8::Handle<Value> args[] = { v8_num(0) };
  recursive_function->Call(env->Global(), 1, args);
  CHECK_EQ(2, callback_fired);
}


void CallCompletedCallbackNoException() {
  v8::HandleScope scope(CcTest::isolate());
  CompileRun("1+1;");
}


void CallCompletedCallbackException() {
  v8::HandleScope scope(CcTest::isolate());
  CompileRun("throw 'second exception';");
}


TEST(CallCompletedCallbackOneException) {
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());
  env->GetIsolate()->AddCallCompletedCallback(CallCompletedCallbackNoException);
  CompileRun("throw 'exception';");
}


TEST(CallCompletedCallbackTwoExceptions) {
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());
  env->GetIsolate()->AddCallCompletedCallback(CallCompletedCallbackException);
  CompileRun("throw 'first exception';");
}


static void MicrotaskOne(const v8::FunctionCallbackInfo<Value>& info) {
  v8::HandleScope scope(info.GetIsolate());
  CompileRun("ext1Calls++;");
}


static void MicrotaskTwo(const v8::FunctionCallbackInfo<Value>& info) {
  v8::HandleScope scope(info.GetIsolate());
  CompileRun("ext2Calls++;");
}


void* g_passed_to_three = NULL;


static void MicrotaskThree(void* data) {
  g_passed_to_three = data;
}


TEST(EnqueueMicrotask) {
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());
  CompileRun(
      "var ext1Calls = 0;"
      "var ext2Calls = 0;");
  CompileRun("1+1;");
  CHECK_EQ(0, CompileRun("ext1Calls")->Int32Value());
  CHECK_EQ(0, CompileRun("ext2Calls")->Int32Value());

  env->GetIsolate()->EnqueueMicrotask(
      Function::New(env->GetIsolate(), MicrotaskOne));
  CompileRun("1+1;");
  CHECK_EQ(1, CompileRun("ext1Calls")->Int32Value());
  CHECK_EQ(0, CompileRun("ext2Calls")->Int32Value());

  env->GetIsolate()->EnqueueMicrotask(
      Function::New(env->GetIsolate(), MicrotaskOne));
  env->GetIsolate()->EnqueueMicrotask(
      Function::New(env->GetIsolate(), MicrotaskTwo));
  CompileRun("1+1;");
  CHECK_EQ(2, CompileRun("ext1Calls")->Int32Value());
  CHECK_EQ(1, CompileRun("ext2Calls")->Int32Value());

  env->GetIsolate()->EnqueueMicrotask(
      Function::New(env->GetIsolate(), MicrotaskTwo));
  CompileRun("1+1;");
  CHECK_EQ(2, CompileRun("ext1Calls")->Int32Value());
  CHECK_EQ(2, CompileRun("ext2Calls")->Int32Value());

  CompileRun("1+1;");
  CHECK_EQ(2, CompileRun("ext1Calls")->Int32Value());
  CHECK_EQ(2, CompileRun("ext2Calls")->Int32Value());

  g_passed_to_three = NULL;
  env->GetIsolate()->EnqueueMicrotask(MicrotaskThree);
  CompileRun("1+1;");
  CHECK(!g_passed_to_three);
  CHECK_EQ(2, CompileRun("ext1Calls")->Int32Value());
  CHECK_EQ(2, CompileRun("ext2Calls")->Int32Value());

  int dummy;
  env->GetIsolate()->EnqueueMicrotask(
      Function::New(env->GetIsolate(), MicrotaskOne));
  env->GetIsolate()->EnqueueMicrotask(MicrotaskThree, &dummy);
  env->GetIsolate()->EnqueueMicrotask(
      Function::New(env->GetIsolate(), MicrotaskTwo));
  CompileRun("1+1;");
  CHECK_EQ(&dummy, g_passed_to_three);
  CHECK_EQ(3, CompileRun("ext1Calls")->Int32Value());
  CHECK_EQ(3, CompileRun("ext2Calls")->Int32Value());
  g_passed_to_three = NULL;
}


static void MicrotaskExceptionOne(
    const v8::FunctionCallbackInfo<Value>& info) {
  v8::HandleScope scope(info.GetIsolate());
  CompileRun("exception1Calls++;");
  info.GetIsolate()->ThrowException(
      v8::Exception::Error(v8_str("first")));
}


static void MicrotaskExceptionTwo(
    const v8::FunctionCallbackInfo<Value>& info) {
  v8::HandleScope scope(info.GetIsolate());
  CompileRun("exception2Calls++;");
  info.GetIsolate()->ThrowException(
      v8::Exception::Error(v8_str("second")));
}


TEST(RunMicrotasksIgnoresThrownExceptions) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);
  CompileRun(
      "var exception1Calls = 0;"
      "var exception2Calls = 0;");
  isolate->EnqueueMicrotask(
      Function::New(isolate, MicrotaskExceptionOne));
  isolate->EnqueueMicrotask(
      Function::New(isolate, MicrotaskExceptionTwo));
  TryCatch try_catch;
  CompileRun("1+1;");
  CHECK(!try_catch.HasCaught());
  CHECK_EQ(1, CompileRun("exception1Calls")->Int32Value());
  CHECK_EQ(1, CompileRun("exception2Calls")->Int32Value());
}


TEST(SetAutorunMicrotasks) {
  LocalContext env;
  v8::HandleScope scope(env->GetIsolate());
  CompileRun(
      "var ext1Calls = 0;"
      "var ext2Calls = 0;");
  CompileRun("1+1;");
  CHECK_EQ(0, CompileRun("ext1Calls")->Int32Value());
  CHECK_EQ(0, CompileRun("ext2Calls")->Int32Value());

  env->GetIsolate()->EnqueueMicrotask(
      Function::New(env->GetIsolate(), MicrotaskOne));
  CompileRun("1+1;");
  CHECK_EQ(1, CompileRun("ext1Calls")->Int32Value());
  CHECK_EQ(0, CompileRun("ext2Calls")->Int32Value());

  env->GetIsolate()->SetAutorunMicrotasks(false);
  env->GetIsolate()->EnqueueMicrotask(
      Function::New(env->GetIsolate(), MicrotaskOne));
  env->GetIsolate()->EnqueueMicrotask(
      Function::New(env->GetIsolate(), MicrotaskTwo));
  CompileRun("1+1;");
  CHECK_EQ(1, CompileRun("ext1Calls")->Int32Value());
  CHECK_EQ(0, CompileRun("ext2Calls")->Int32Value());

  env->GetIsolate()->RunMicrotasks();
  CHECK_EQ(2, CompileRun("ext1Calls")->Int32Value());
  CHECK_EQ(1, CompileRun("ext2Calls")->Int32Value());

  env->GetIsolate()->EnqueueMicrotask(
      Function::New(env->GetIsolate(), MicrotaskTwo));
  CompileRun("1+1;");
  CHECK_EQ(2, CompileRun("ext1Calls")->Int32Value());
  CHECK_EQ(1, CompileRun("ext2Calls")->Int32Value());

  env->GetIsolate()->RunMicrotasks();
  CHECK_EQ(2, CompileRun("ext1Calls")->Int32Value());
  CHECK_EQ(2, CompileRun("ext2Calls")->Int32Value());

  env->GetIsolate()->SetAutorunMicrotasks(true);
  env->GetIsolate()->EnqueueMicrotask(
      Function::New(env->GetIsolate(), MicrotaskTwo));
  CompileRun("1+1;");
  CHECK_EQ(2, CompileRun("ext1Calls")->Int32Value());
  CHECK_EQ(3, CompileRun("ext2Calls")->Int32Value());

  env->GetIsolate()->EnqueueMicrotask(
      Function::New(env->GetIsolate(), MicrotaskTwo));
  {
    v8::Isolate::SuppressMicrotaskExecutionScope scope(env->GetIsolate());
    CompileRun("1+1;");
    CHECK_EQ(2, CompileRun("ext1Calls")->Int32Value());
    CHECK_EQ(3, CompileRun("ext2Calls")->Int32Value());
  }

  CompileRun("1+1;");
  CHECK_EQ(2, CompileRun("ext1Calls")->Int32Value());
  CHECK_EQ(4, CompileRun("ext2Calls")->Int32Value());
}


TEST(RunMicrotasksWithoutEnteringContext) {
  v8::Isolate* isolate = CcTest::isolate();
  HandleScope handle_scope(isolate);
  isolate->SetAutorunMicrotasks(false);
  Handle<Context> context = Context::New(isolate);
  {
    Context::Scope context_scope(context);
    CompileRun("var ext1Calls = 0;");
    isolate->EnqueueMicrotask(Function::New(isolate, MicrotaskOne));
  }
  isolate->RunMicrotasks();
  {
    Context::Scope context_scope(context);
    CHECK_EQ(1, CompileRun("ext1Calls")->Int32Value());
  }
  isolate->SetAutorunMicrotasks(true);
}


static void DebugEventInObserver(const v8::Debug::EventDetails& event_details) {
  v8::DebugEvent event = event_details.GetEvent();
  if (event != v8::Break) return;
  Handle<Object> exec_state = event_details.GetExecutionState();
  Handle<Value> break_id = exec_state->Get(v8_str("break_id"));
  CompileRun("function f(id) { new FrameDetails(id, 0); }");
  Handle<Function> fun =
      Handle<Function>::Cast(CcTest::global()->Get(v8_str("f")));
  fun->Call(CcTest::global(), 1, &break_id);
}


TEST(Regress385349) {
  i::FLAG_allow_natives_syntax = true;
  v8::Isolate* isolate = CcTest::isolate();
  HandleScope handle_scope(isolate);
  isolate->SetAutorunMicrotasks(false);
  Handle<Context> context = Context::New(isolate);
  v8::Debug::SetDebugEventListener(DebugEventInObserver);
  {
    Context::Scope context_scope(context);
    CompileRun("var obj = {};"
               "Object.observe(obj, function(changes) { debugger; });"
               "obj.a = 0;");
  }
  isolate->RunMicrotasks();
  isolate->SetAutorunMicrotasks(true);
  v8::Debug::SetDebugEventListener(NULL);
}


#ifdef ENABLE_DISASSEMBLER
static int probes_counter = 0;
static int misses_counter = 0;
static int updates_counter = 0;


static int* LookupCounter(const char* name) {
  if (strcmp(name, "c:V8.MegamorphicStubCacheProbes") == 0) {
    return &probes_counter;
  } else if (strcmp(name, "c:V8.MegamorphicStubCacheMisses") == 0) {
    return &misses_counter;
  } else if (strcmp(name, "c:V8.MegamorphicStubCacheUpdates") == 0) {
    return &updates_counter;
  }
  return NULL;
}


static const char* kMegamorphicTestProgram =
    "function ClassA() { };"
    "function ClassB() { };"
    "ClassA.prototype.foo = function() { };"
    "ClassB.prototype.foo = function() { };"
    "function fooify(obj) { obj.foo(); };"
    "var a = new ClassA();"
    "var b = new ClassB();"
    "for (var i = 0; i < 10000; i++) {"
    "  fooify(a);"
    "  fooify(b);"
    "}";
#endif


static void StubCacheHelper(bool primary) {
#ifdef ENABLE_DISASSEMBLER
  i::FLAG_native_code_counters = true;
  if (primary) {
    i::FLAG_test_primary_stub_cache = true;
  } else {
    i::FLAG_test_secondary_stub_cache = true;
  }
  i::FLAG_crankshaft = false;
  LocalContext env;
  env->GetIsolate()->SetCounterFunction(LookupCounter);
  v8::HandleScope scope(env->GetIsolate());
  int initial_probes = probes_counter;
  int initial_misses = misses_counter;
  int initial_updates = updates_counter;
  CompileRun(kMegamorphicTestProgram);
  int probes = probes_counter - initial_probes;
  int misses = misses_counter - initial_misses;
  int updates = updates_counter - initial_updates;
  CHECK_LT(updates, 10);
  CHECK_LT(misses, 10);
  // TODO(verwaest): Update this test to overflow the degree of polymorphism
  // before megamorphism. The number of probes will only work once we teach the
  // serializer to embed references to counters in the stubs, given that the
  // megamorphic_stub_cache_probes is updated in a snapshot-generated stub.
  CHECK_GE(probes, 0);
#endif
}


TEST(SecondaryStubCache) {
  StubCacheHelper(true);
}


TEST(PrimaryStubCache) {
  StubCacheHelper(false);
}


#ifdef DEBUG
static int cow_arrays_created_runtime = 0;


static int* LookupCounterCOWArrays(const char* name) {
  if (strcmp(name, "c:V8.COWArraysCreatedRuntime") == 0) {
    return &cow_arrays_created_runtime;
  }
  return NULL;
}
#endif


TEST(CheckCOWArraysCreatedRuntimeCounter) {
#ifdef DEBUG
  i::FLAG_native_code_counters = true;
  LocalContext env;
  env->GetIsolate()->SetCounterFunction(LookupCounterCOWArrays);
  v8::HandleScope scope(env->GetIsolate());
  int initial_cow_arrays = cow_arrays_created_runtime;
  CompileRun("var o = [1, 2, 3];");
  CHECK_EQ(1, cow_arrays_created_runtime - initial_cow_arrays);
  CompileRun("var o = {foo: [4, 5, 6], bar: [3, 0]};");
  CHECK_EQ(3, cow_arrays_created_runtime - initial_cow_arrays);
  CompileRun("var o = {foo: [1, 2, 3, [4, 5, 6]], bar: 'hi'};");
  CHECK_EQ(4, cow_arrays_created_runtime - initial_cow_arrays);
#endif
}


TEST(StaticGetters) {
  LocalContext context;
  i::Factory* factory = CcTest::i_isolate()->factory();
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  i::Handle<i::Object> undefined_value = factory->undefined_value();
  CHECK(*v8::Utils::OpenHandle(*v8::Undefined(isolate)) == *undefined_value);
  i::Handle<i::Object> null_value = factory->null_value();
  CHECK(*v8::Utils::OpenHandle(*v8::Null(isolate)) == *null_value);
  i::Handle<i::Object> true_value = factory->true_value();
  CHECK(*v8::Utils::OpenHandle(*v8::True(isolate)) == *true_value);
  i::Handle<i::Object> false_value = factory->false_value();
  CHECK(*v8::Utils::OpenHandle(*v8::False(isolate)) == *false_value);
}


UNINITIALIZED_TEST(IsolateEmbedderData) {
  CcTest::DisableAutomaticDispose();
  v8::Isolate* isolate = v8::Isolate::New();
  isolate->Enter();
  i::Isolate* i_isolate = reinterpret_cast<i::Isolate*>(isolate);
  for (uint32_t slot = 0; slot < v8::Isolate::GetNumberOfDataSlots(); ++slot) {
    CHECK(!isolate->GetData(slot));
    CHECK(!i_isolate->GetData(slot));
  }
  for (uint32_t slot = 0; slot < v8::Isolate::GetNumberOfDataSlots(); ++slot) {
    void* data = reinterpret_cast<void*>(0xacce55ed + slot);
    isolate->SetData(slot, data);
  }
  for (uint32_t slot = 0; slot < v8::Isolate::GetNumberOfDataSlots(); ++slot) {
    void* data = reinterpret_cast<void*>(0xacce55ed + slot);
    CHECK_EQ(data, isolate->GetData(slot));
    CHECK_EQ(data, i_isolate->GetData(slot));
  }
  for (uint32_t slot = 0; slot < v8::Isolate::GetNumberOfDataSlots(); ++slot) {
    void* data = reinterpret_cast<void*>(0xdecea5ed + slot);
    isolate->SetData(slot, data);
  }
  for (uint32_t slot = 0; slot < v8::Isolate::GetNumberOfDataSlots(); ++slot) {
    void* data = reinterpret_cast<void*>(0xdecea5ed + slot);
    CHECK_EQ(data, isolate->GetData(slot));
    CHECK_EQ(data, i_isolate->GetData(slot));
  }
  isolate->Exit();
  isolate->Dispose();
}


TEST(StringEmpty) {
  LocalContext context;
  i::Factory* factory = CcTest::i_isolate()->factory();
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  i::Handle<i::Object> empty_string = factory->empty_string();
  CHECK(*v8::Utils::OpenHandle(*v8::String::Empty(isolate)) == *empty_string);
}


static int instance_checked_getter_count = 0;
static void InstanceCheckedGetter(
    Local<String> name,
    const v8::PropertyCallbackInfo<v8::Value>& info) {
  CHECK(name->Equals(v8_str("foo")));
  instance_checked_getter_count++;
  info.GetReturnValue().Set(v8_num(11));
}


static int instance_checked_setter_count = 0;
static void InstanceCheckedSetter(Local<String> name,
                      Local<Value> value,
                      const v8::PropertyCallbackInfo<void>& info) {
  CHECK(name->Equals(v8_str("foo")));
  CHECK(value->Equals(v8_num(23)));
  instance_checked_setter_count++;
}


static void CheckInstanceCheckedResult(int getters, int setters,
                                       bool expects_callbacks,
                                       TryCatch* try_catch) {
  if (expects_callbacks) {
    CHECK(!try_catch->HasCaught());
    CHECK_EQ(getters, instance_checked_getter_count);
    CHECK_EQ(setters, instance_checked_setter_count);
  } else {
    CHECK(try_catch->HasCaught());
    CHECK_EQ(0, instance_checked_getter_count);
    CHECK_EQ(0, instance_checked_setter_count);
  }
  try_catch->Reset();
}


static void CheckInstanceCheckedAccessors(bool expects_callbacks) {
  instance_checked_getter_count = 0;
  instance_checked_setter_count = 0;
  TryCatch try_catch;

  // Test path through generic runtime code.
  CompileRun("obj.foo");
  CheckInstanceCheckedResult(1, 0, expects_callbacks, &try_catch);
  CompileRun("obj.foo = 23");
  CheckInstanceCheckedResult(1, 1, expects_callbacks, &try_catch);

  // Test path through generated LoadIC and StoredIC.
  CompileRun("function test_get(o) { o.foo; }"
             "test_get(obj);");
  CheckInstanceCheckedResult(2, 1, expects_callbacks, &try_catch);
  CompileRun("test_get(obj);");
  CheckInstanceCheckedResult(3, 1, expects_callbacks, &try_catch);
  CompileRun("test_get(obj);");
  CheckInstanceCheckedResult(4, 1, expects_callbacks, &try_catch);
  CompileRun("function test_set(o) { o.foo = 23; }"
             "test_set(obj);");
  CheckInstanceCheckedResult(4, 2, expects_callbacks, &try_catch);
  CompileRun("test_set(obj);");
  CheckInstanceCheckedResult(4, 3, expects_callbacks, &try_catch);
  CompileRun("test_set(obj);");
  CheckInstanceCheckedResult(4, 4, expects_callbacks, &try_catch);

  // Test path through optimized code.
  CompileRun("%OptimizeFunctionOnNextCall(test_get);"
             "test_get(obj);");
  CheckInstanceCheckedResult(5, 4, expects_callbacks, &try_catch);
  CompileRun("%OptimizeFunctionOnNextCall(test_set);"
             "test_set(obj);");
  CheckInstanceCheckedResult(5, 5, expects_callbacks, &try_catch);

  // Cleanup so that closures start out fresh in next check.
  CompileRun("%DeoptimizeFunction(test_get);"
             "%ClearFunctionTypeFeedback(test_get);"
             "%DeoptimizeFunction(test_set);"
             "%ClearFunctionTypeFeedback(test_set);");
}


THREADED_TEST(InstanceCheckOnInstanceAccessor) {
  v8::internal::FLAG_allow_natives_syntax = true;
  LocalContext context;
  v8::HandleScope scope(context->GetIsolate());

  Local<FunctionTemplate> templ = FunctionTemplate::New(context->GetIsolate());
  Local<ObjectTemplate> inst = templ->InstanceTemplate();
  inst->SetAccessor(v8_str("foo"),
                    InstanceCheckedGetter, InstanceCheckedSetter,
                    Handle<Value>(),
                    v8::DEFAULT,
                    v8::None,
                    v8::AccessorSignature::New(context->GetIsolate(), templ));
  context->Global()->Set(v8_str("f"), templ->GetFunction());

  printf("Testing positive ...\n");
  CompileRun("var obj = new f();");
  CHECK(templ->HasInstance(context->Global()->Get(v8_str("obj"))));
  CheckInstanceCheckedAccessors(true);

  printf("Testing negative ...\n");
  CompileRun("var obj = {};"
             "obj.__proto__ = new f();");
  CHECK(!templ->HasInstance(context->Global()->Get(v8_str("obj"))));
  CheckInstanceCheckedAccessors(false);
}


static void EmptyInterceptorGetter(
    Local<String> name, const v8::PropertyCallbackInfo<v8::Value>& info) {}


static void EmptyInterceptorSetter(
    Local<String> name, Local<Value> value,
    const v8::PropertyCallbackInfo<v8::Value>& info) {}


THREADED_TEST(InstanceCheckOnInstanceAccessorWithInterceptor) {
  v8::internal::FLAG_allow_natives_syntax = true;
  LocalContext context;
  v8::HandleScope scope(context->GetIsolate());

  Local<FunctionTemplate> templ = FunctionTemplate::New(context->GetIsolate());
  Local<ObjectTemplate> inst = templ->InstanceTemplate();
  templ->InstanceTemplate()->SetNamedPropertyHandler(EmptyInterceptorGetter,
                                                     EmptyInterceptorSetter);
  inst->SetAccessor(v8_str("foo"),
                    InstanceCheckedGetter, InstanceCheckedSetter,
                    Handle<Value>(),
                    v8::DEFAULT,
                    v8::None,
                    v8::AccessorSignature::New(context->GetIsolate(), templ));
  context->Global()->Set(v8_str("f"), templ->GetFunction());

  printf("Testing positive ...\n");
  CompileRun("var obj = new f();");
  CHECK(templ->HasInstance(context->Global()->Get(v8_str("obj"))));
  CheckInstanceCheckedAccessors(true);

  printf("Testing negative ...\n");
  CompileRun("var obj = {};"
             "obj.__proto__ = new f();");
  CHECK(!templ->HasInstance(context->Global()->Get(v8_str("obj"))));
  CheckInstanceCheckedAccessors(false);
}


THREADED_TEST(InstanceCheckOnPrototypeAccessor) {
  v8::internal::FLAG_allow_natives_syntax = true;
  LocalContext context;
  v8::HandleScope scope(context->GetIsolate());

  Local<FunctionTemplate> templ = FunctionTemplate::New(context->GetIsolate());
  Local<ObjectTemplate> proto = templ->PrototypeTemplate();
  proto->SetAccessor(v8_str("foo"), InstanceCheckedGetter,
                     InstanceCheckedSetter, Handle<Value>(), v8::DEFAULT,
                     v8::None,
                     v8::AccessorSignature::New(context->GetIsolate(), templ));
  context->Global()->Set(v8_str("f"), templ->GetFunction());

  printf("Testing positive ...\n");
  CompileRun("var obj = new f();");
  CHECK(templ->HasInstance(context->Global()->Get(v8_str("obj"))));
  CheckInstanceCheckedAccessors(true);

  printf("Testing negative ...\n");
  CompileRun("var obj = {};"
             "obj.__proto__ = new f();");
  CHECK(!templ->HasInstance(context->Global()->Get(v8_str("obj"))));
  CheckInstanceCheckedAccessors(false);

  printf("Testing positive with modified prototype chain ...\n");
  CompileRun("var obj = new f();"
             "var pro = {};"
             "pro.__proto__ = obj.__proto__;"
             "obj.__proto__ = pro;");
  CHECK(templ->HasInstance(context->Global()->Get(v8_str("obj"))));
  CheckInstanceCheckedAccessors(true);
}


TEST(TryFinallyMessage) {
  LocalContext context;
  v8::HandleScope scope(context->GetIsolate());
  {
    // Test that the original error message is not lost if there is a
    // recursive call into Javascript is done in the finally block, e.g. to
    // initialize an IC. (crbug.com/129171)
    TryCatch try_catch;
    const char* trigger_ic =
        "try {                      \n"
        "  throw new Error('test'); \n"
        "} finally {                \n"
        "  var x = 0;               \n"
        "  x++;                     \n"  // Trigger an IC initialization here.
        "}                          \n";
    CompileRun(trigger_ic);
    CHECK(try_catch.HasCaught());
    Local<Message> message = try_catch.Message();
    CHECK(!message.IsEmpty());
    CHECK_EQ(2, message->GetLineNumber());
  }

  {
    // Test that the original exception message is indeed overwritten if
    // a new error is thrown in the finally block.
    TryCatch try_catch;
    const char* throw_again =
        "try {                       \n"
        "  throw new Error('test');  \n"
        "} finally {                 \n"
        "  var x = 0;                \n"
        "  x++;                      \n"
        "  throw new Error('again'); \n"  // This is the new uncaught error.
        "}                           \n";
    CompileRun(throw_again);
    CHECK(try_catch.HasCaught());
    Local<Message> message = try_catch.Message();
    CHECK(!message.IsEmpty());
    CHECK_EQ(6, message->GetLineNumber());
  }
}


static void Helper137002(bool do_store,
                         bool polymorphic,
                         bool remove_accessor,
                         bool interceptor) {
  LocalContext context;
  Local<ObjectTemplate> templ = ObjectTemplate::New(context->GetIsolate());
  if (interceptor) {
    templ->SetHandler(v8::NamedPropertyHandlerConfiguration(FooGetInterceptor,
                                                            FooSetInterceptor));
  } else {
    templ->SetAccessor(v8_str("foo"),
                       GetterWhichReturns42,
                       SetterWhichSetsYOnThisTo23);
  }
  context->Global()->Set(v8_str("obj"), templ->NewInstance());

  // Turn monomorphic on slow object with native accessor, then turn
  // polymorphic, finally optimize to create negative lookup and fail.
  CompileRun(do_store ?
             "function f(x) { x.foo = void 0; }" :
             "function f(x) { return x.foo; }");
  CompileRun("obj.y = void 0;");
  if (!interceptor) {
    CompileRun("%OptimizeObjectForAddingMultipleProperties(obj, 1);");
  }
  CompileRun("obj.__proto__ = null;"
             "f(obj); f(obj); f(obj);");
  if (polymorphic) {
    CompileRun("f({});");
  }
  CompileRun("obj.y = void 0;"
             "%OptimizeFunctionOnNextCall(f);");
  if (remove_accessor) {
    CompileRun("delete obj.foo;");
  }
  CompileRun("var result = f(obj);");
  if (do_store) {
    CompileRun("result = obj.y;");
  }
  if (remove_accessor && !interceptor) {
    CHECK(context->Global()->Get(v8_str("result"))->IsUndefined());
  } else {
    CHECK_EQ(do_store ? 23 : 42,
             context->Global()->Get(v8_str("result"))->Int32Value());
  }
}


THREADED_TEST(Regress137002a) {
  i::FLAG_allow_natives_syntax = true;
  i::FLAG_compilation_cache = false;
  v8::HandleScope scope(CcTest::isolate());
  for (int i = 0; i < 16; i++) {
    Helper137002(i & 8, i & 4, i & 2, i & 1);
  }
}


THREADED_TEST(Regress137002b) {
  i::FLAG_allow_natives_syntax = true;
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);
  Local<ObjectTemplate> templ = ObjectTemplate::New(isolate);
  templ->SetAccessor(v8_str("foo"),
                     GetterWhichReturns42,
                     SetterWhichSetsYOnThisTo23);
  context->Global()->Set(v8_str("obj"), templ->NewInstance());

  // Turn monomorphic on slow object with native accessor, then just
  // delete the property and fail.
  CompileRun("function load(x) { return x.foo; }"
             "function store(x) { x.foo = void 0; }"
             "function keyed_load(x, key) { return x[key]; }"
             // Second version of function has a different source (add void 0)
             // so that it does not share code with the first version.  This
             // ensures that the ICs are monomorphic.
             "function load2(x) { void 0; return x.foo; }"
             "function store2(x) { void 0; x.foo = void 0; }"
             "function keyed_load2(x, key) { void 0; return x[key]; }"

             "obj.y = void 0;"
             "obj.__proto__ = null;"
             "var subobj = {};"
             "subobj.y = void 0;"
             "subobj.__proto__ = obj;"
             "%OptimizeObjectForAddingMultipleProperties(obj, 1);"

             // Make the ICs monomorphic.
             "load(obj); load(obj);"
             "load2(subobj); load2(subobj);"
             "store(obj); store(obj);"
             "store2(subobj); store2(subobj);"
             "keyed_load(obj, 'foo'); keyed_load(obj, 'foo');"
             "keyed_load2(subobj, 'foo'); keyed_load2(subobj, 'foo');"

             // Actually test the shiny new ICs and better not crash. This
             // serves as a regression test for issue 142088 as well.
             "load(obj);"
             "load2(subobj);"
             "store(obj);"
             "store2(subobj);"
             "keyed_load(obj, 'foo');"
             "keyed_load2(subobj, 'foo');"

             // Delete the accessor.  It better not be called any more now.
             "delete obj.foo;"
             "obj.y = void 0;"
             "subobj.y = void 0;"

             "var load_result = load(obj);"
             "var load_result2 = load2(subobj);"
             "var keyed_load_result = keyed_load(obj, 'foo');"
             "var keyed_load_result2 = keyed_load2(subobj, 'foo');"
             "store(obj);"
             "store2(subobj);"
             "var y_from_obj = obj.y;"
             "var y_from_subobj = subobj.y;");
  CHECK(context->Global()->Get(v8_str("load_result"))->IsUndefined());
  CHECK(context->Global()->Get(v8_str("load_result2"))->IsUndefined());
  CHECK(context->Global()->Get(v8_str("keyed_load_result"))->IsUndefined());
  CHECK(context->Global()->Get(v8_str("keyed_load_result2"))->IsUndefined());
  CHECK(context->Global()->Get(v8_str("y_from_obj"))->IsUndefined());
  CHECK(context->Global()->Get(v8_str("y_from_subobj"))->IsUndefined());
}


THREADED_TEST(Regress142088) {
  i::FLAG_allow_natives_syntax = true;
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);
  Local<ObjectTemplate> templ = ObjectTemplate::New(isolate);
  templ->SetAccessor(v8_str("foo"),
                     GetterWhichReturns42,
                     SetterWhichSetsYOnThisTo23);
  context->Global()->Set(v8_str("obj"), templ->NewInstance());

  CompileRun("function load(x) { return x.foo; }"
             "var o = Object.create(obj);"
             "%OptimizeObjectForAddingMultipleProperties(obj, 1);"
             "load(o); load(o); load(o); load(o);");
}


THREADED_TEST(Regress3337) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);
  Local<v8::Object> o1 = Object::New(isolate);
  Local<v8::Object> o2 = Object::New(isolate);
  i::Handle<i::JSObject> io1 = v8::Utils::OpenHandle(*o1);
  i::Handle<i::JSObject> io2 = v8::Utils::OpenHandle(*o2);
  CHECK(io1->map() == io2->map());
  o1->SetIndexedPropertiesToExternalArrayData(
      NULL, v8::kExternalUint32Array, 0);
  o2->SetIndexedPropertiesToExternalArrayData(
      NULL, v8::kExternalUint32Array, 0);
  CHECK(io1->map() == io2->map());
}


THREADED_TEST(Regress137496) {
  i::FLAG_expose_gc = true;
  LocalContext context;
  v8::HandleScope scope(context->GetIsolate());

  // Compile a try-finally clause where the finally block causes a GC
  // while there still is a message pending for external reporting.
  TryCatch try_catch;
  try_catch.SetVerbose(true);
  CompileRun("try { throw new Error(); } finally { gc(); }");
  CHECK(try_catch.HasCaught());
}


THREADED_TEST(Regress157124) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);
  Local<ObjectTemplate> templ = ObjectTemplate::New(isolate);
  Local<Object> obj = templ->NewInstance();
  obj->GetIdentityHash();
  obj->DeleteHiddenValue(v8_str("Bug"));
}


THREADED_TEST(Regress2535) {
  LocalContext context;
  v8::HandleScope scope(context->GetIsolate());
  Local<Value> set_value = CompileRun("new Set();");
  Local<Object> set_object(Local<Object>::Cast(set_value));
  CHECK_EQ(0, set_object->InternalFieldCount());
  Local<Value> map_value = CompileRun("new Map();");
  Local<Object> map_object(Local<Object>::Cast(map_value));
  CHECK_EQ(0, map_object->InternalFieldCount());
}


THREADED_TEST(Regress2746) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);
  Local<Object> obj = Object::New(isolate);
  Local<String> key = String::NewFromUtf8(context->GetIsolate(), "key");
  obj->SetHiddenValue(key, v8::Undefined(isolate));
  Local<Value> value = obj->GetHiddenValue(key);
  CHECK(!value.IsEmpty());
  CHECK(value->IsUndefined());
}


THREADED_TEST(Regress260106) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);
  Local<FunctionTemplate> templ = FunctionTemplate::New(isolate,
                                                        DummyCallHandler);
  CompileRun("for (var i = 0; i < 128; i++) Object.prototype[i] = 0;");
  Local<Function> function = templ->GetFunction();
  CHECK(!function.IsEmpty());
  CHECK(function->IsFunction());
}


THREADED_TEST(JSONParseObject) {
  LocalContext context;
  HandleScope scope(context->GetIsolate());
  Local<Value> obj = v8::JSON::Parse(v8_str("{\"x\":42}"));
  Handle<Object> global = context->Global();
  global->Set(v8_str("obj"), obj);
  ExpectString("JSON.stringify(obj)", "{\"x\":42}");
}


THREADED_TEST(JSONParseNumber) {
  LocalContext context;
  HandleScope scope(context->GetIsolate());
  Local<Value> obj = v8::JSON::Parse(v8_str("42"));
  Handle<Object> global = context->Global();
  global->Set(v8_str("obj"), obj);
  ExpectString("JSON.stringify(obj)", "42");
}


#if V8_OS_POSIX && !V8_OS_NACL
class ThreadInterruptTest {
 public:
  ThreadInterruptTest() : sem_(0), sem_value_(0) { }
  ~ThreadInterruptTest() {}

  void RunTest() {
    InterruptThread i_thread(this);
    i_thread.Start();

    sem_.Wait();
    CHECK_EQ(kExpectedValue, sem_value_);
  }

 private:
  static const int kExpectedValue = 1;

  class InterruptThread : public v8::base::Thread {
   public:
    explicit InterruptThread(ThreadInterruptTest* test)
        : Thread(Options("InterruptThread")), test_(test) {}

    virtual void Run() {
      struct sigaction action;

      // Ensure that we'll enter waiting condition
      v8::base::OS::Sleep(100);

      // Setup signal handler
      memset(&action, 0, sizeof(action));
      action.sa_handler = SignalHandler;
      sigaction(SIGCHLD, &action, NULL);

      // Send signal
      kill(getpid(), SIGCHLD);

      // Ensure that if wait has returned because of error
      v8::base::OS::Sleep(100);

      // Set value and signal semaphore
      test_->sem_value_ = 1;
      test_->sem_.Signal();
    }

    static void SignalHandler(int signal) {
    }

   private:
     ThreadInterruptTest* test_;
  };

  v8::base::Semaphore sem_;
  volatile int sem_value_;
};


THREADED_TEST(SemaphoreInterruption) {
  ThreadInterruptTest().RunTest();
}


#endif  // V8_OS_POSIX


void UnreachableCallback(const v8::FunctionCallbackInfo<v8::Value>& args) {
  CHECK(false);
}


TEST(JSONStringifyAccessCheck) {
  v8::V8::Initialize();
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);

  // Create an ObjectTemplate for global objects and install access
  // check callbacks that will block access.
  v8::Handle<v8::ObjectTemplate> global_template =
      v8::ObjectTemplate::New(isolate);
  global_template->SetAccessCheckCallbacks(AccessAlwaysBlocked, NULL);

  // Create a context and set an x property on it's global object.
  LocalContext context0(NULL, global_template);
  v8::Handle<v8::Object> global0 = context0->Global();
  global0->Set(v8_str("x"), v8_num(42));
  ExpectString("JSON.stringify(this)", "{\"x\":42}");

  for (int i = 0; i < 2; i++) {
    if (i == 1) {
      // Install a toJSON function on the second run.
      v8::Handle<v8::FunctionTemplate> toJSON =
          v8::FunctionTemplate::New(isolate, UnreachableCallback);

      global0->Set(v8_str("toJSON"), toJSON->GetFunction());
    }
    // Create a context with a different security token so that the
    // failed access check callback will be called on each access.
    LocalContext context1(NULL, global_template);
    context1->Global()->Set(v8_str("other"), global0);

    CHECK(CompileRun("JSON.stringify(other)").IsEmpty());
    CHECK(CompileRun("JSON.stringify({ 'a' : other, 'b' : ['c'] })").IsEmpty());
    CHECK(CompileRun("JSON.stringify([other, 'b', 'c'])").IsEmpty());

    v8::Handle<v8::Array> array = v8::Array::New(isolate, 2);
    array->Set(0, v8_str("a"));
    array->Set(1, v8_str("b"));
    context1->Global()->Set(v8_str("array"), array);
    ExpectString("JSON.stringify(array)", "[\"a\",\"b\"]");
    array->TurnOnAccessCheck();
    CHECK(CompileRun("JSON.stringify(array)").IsEmpty());
    CHECK(CompileRun("JSON.stringify([array])").IsEmpty());
    CHECK(CompileRun("JSON.stringify({'a' : array})").IsEmpty());
  }
}


bool access_check_fail_thrown = false;
bool catch_callback_called = false;


// Failed access check callback that performs a GC on each invocation.
void FailedAccessCheckThrows(Local<v8::Object> target,
                             v8::AccessType type,
                             Local<v8::Value> data) {
  access_check_fail_thrown = true;
  i::PrintF("Access check failed. Error thrown.\n");
  CcTest::isolate()->ThrowException(
      v8::Exception::Error(v8_str("cross context")));
}


void CatcherCallback(const v8::FunctionCallbackInfo<v8::Value>& args) {
  for (int i = 0; i < args.Length(); i++) {
    i::PrintF("%s\n", *String::Utf8Value(args[i]));
  }
  catch_callback_called = true;
}


void HasOwnPropertyCallback(const v8::FunctionCallbackInfo<v8::Value>& args) {
  args[0]->ToObject(args.GetIsolate())->HasOwnProperty(
      args[1]->ToString(args.GetIsolate()));
}


void CheckCorrectThrow(const char* script) {
  // Test that the script, when wrapped into a try-catch, triggers the catch
  // clause due to failed access check throwing an exception.
  // The subsequent try-catch should run without any exception.
  access_check_fail_thrown = false;
  catch_callback_called = false;
  i::ScopedVector<char> source(1024);
  i::SNPrintF(source, "try { %s; } catch (e) { catcher(e); }", script);
  CompileRun(source.start());
  CHECK(access_check_fail_thrown);
  CHECK(catch_callback_called);

  access_check_fail_thrown = false;
  catch_callback_called = false;
  CompileRun("try { [1, 2, 3].sort(); } catch (e) { catcher(e) };");
  CHECK(!access_check_fail_thrown);
  CHECK(!catch_callback_called);
}


TEST(AccessCheckThrows) {
  i::FLAG_allow_natives_syntax = true;
  v8::V8::Initialize();
  v8::V8::SetFailedAccessCheckCallbackFunction(&FailedAccessCheckThrows);
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);

  // Create an ObjectTemplate for global objects and install access
  // check callbacks that will block access.
  v8::Handle<v8::ObjectTemplate> global_template =
      v8::ObjectTemplate::New(isolate);
  global_template->SetAccessCheckCallbacks(AccessAlwaysBlocked, NULL);

  // Create a context and set an x property on it's global object.
  LocalContext context0(NULL, global_template);
  v8::Handle<v8::Object> global0 = context0->Global();

  // Create a context with a different security token so that the
  // failed access check callback will be called on each access.
  LocalContext context1(NULL, global_template);
  context1->Global()->Set(v8_str("other"), global0);

  v8::Handle<v8::FunctionTemplate> catcher_fun =
      v8::FunctionTemplate::New(isolate, CatcherCallback);
  context1->Global()->Set(v8_str("catcher"), catcher_fun->GetFunction());

  v8::Handle<v8::FunctionTemplate> has_own_property_fun =
      v8::FunctionTemplate::New(isolate, HasOwnPropertyCallback);
  context1->Global()->Set(v8_str("has_own_property"),
                          has_own_property_fun->GetFunction());

  { v8::TryCatch try_catch;
    access_check_fail_thrown = false;
    CompileRun("other.x;");
    CHECK(access_check_fail_thrown);
    CHECK(try_catch.HasCaught());
  }

  CheckCorrectThrow("other.x");
  CheckCorrectThrow("other[1]");
  CheckCorrectThrow("JSON.stringify(other)");
  CheckCorrectThrow("has_own_property(other, 'x')");
  CheckCorrectThrow("%GetProperty(other, 'x')");
  CheckCorrectThrow("%SetProperty(other, 'x', 'foo', 0)");
  CheckCorrectThrow("%AddNamedProperty(other, 'x', 'foo', 1)");
  CheckCorrectThrow("%DeleteProperty(other, 'x', 0)");
  CheckCorrectThrow("%DeleteProperty(other, '1', 0)");
  CheckCorrectThrow("%HasOwnProperty(other, 'x')");
  CheckCorrectThrow("%HasProperty(other, 'x')");
  CheckCorrectThrow("%HasElement(other, 1)");
  CheckCorrectThrow("%IsPropertyEnumerable(other, 'x')");
  CheckCorrectThrow("%GetPropertyNames(other)");
  // PROPERTY_ATTRIBUTES_NONE = 0
  CheckCorrectThrow("%GetOwnPropertyNames(other, 0)");
  CheckCorrectThrow("%DefineAccessorPropertyUnchecked("
                        "other, 'x', null, null, 1)");

  // Reset the failed access check callback so it does not influence
  // the other tests.
  v8::V8::SetFailedAccessCheckCallbackFunction(NULL);
}


class RequestInterruptTestBase {
 public:
  RequestInterruptTestBase()
      : env_(),
        isolate_(env_->GetIsolate()),
        sem_(0),
        warmup_(20000),
        should_continue_(true) {
  }

  virtual ~RequestInterruptTestBase() { }

  virtual void StartInterruptThread() = 0;

  virtual void TestBody() = 0;

  void RunTest() {
    StartInterruptThread();

    v8::HandleScope handle_scope(isolate_);

    TestBody();

    // Verify we arrived here because interruptor was called
    // not due to a bug causing us to exit the loop too early.
    CHECK(!should_continue());
  }

  void WakeUpInterruptor() {
    sem_.Signal();
  }

  bool should_continue() const { return should_continue_; }

  bool ShouldContinue() {
    if (warmup_ > 0) {
      if (--warmup_ == 0) {
        WakeUpInterruptor();
      }
    }

    return should_continue_;
  }

  static void ShouldContinueCallback(
      const v8::FunctionCallbackInfo<Value>& info) {
    RequestInterruptTestBase* test =
        reinterpret_cast<RequestInterruptTestBase*>(
            info.Data().As<v8::External>()->Value());
    info.GetReturnValue().Set(test->ShouldContinue());
  }

  LocalContext env_;
  v8::Isolate* isolate_;
  v8::base::Semaphore sem_;
  int warmup_;
  bool should_continue_;
};


class RequestInterruptTestBaseWithSimpleInterrupt
    : public RequestInterruptTestBase {
 public:
  RequestInterruptTestBaseWithSimpleInterrupt() : i_thread(this) { }

  virtual void StartInterruptThread() {
    i_thread.Start();
  }

 private:
  class InterruptThread : public v8::base::Thread {
   public:
    explicit InterruptThread(RequestInterruptTestBase* test)
        : Thread(Options("RequestInterruptTest")), test_(test) {}

    virtual void Run() {
      test_->sem_.Wait();
      test_->isolate_->RequestInterrupt(&OnInterrupt, test_);
    }

    static void OnInterrupt(v8::Isolate* isolate, void* data) {
      reinterpret_cast<RequestInterruptTestBase*>(data)->
          should_continue_ = false;
    }

   private:
     RequestInterruptTestBase* test_;
  };

  InterruptThread i_thread;
};


class RequestInterruptTestWithFunctionCall
    : public RequestInterruptTestBaseWithSimpleInterrupt {
 public:
  virtual void TestBody() {
    Local<Function> func = Function::New(
        isolate_, ShouldContinueCallback, v8::External::New(isolate_, this));
    env_->Global()->Set(v8_str("ShouldContinue"), func);

    i::FLAG_turbo_osr = false;  // TODO(titzer): interrupts in TF loops.
    CompileRun("while (ShouldContinue()) { }");
  }
};


class RequestInterruptTestWithMethodCall
    : public RequestInterruptTestBaseWithSimpleInterrupt {
 public:
  virtual void TestBody() {
    v8::Local<v8::FunctionTemplate> t = v8::FunctionTemplate::New(isolate_);
    v8::Local<v8::Template> proto = t->PrototypeTemplate();
    proto->Set(v8_str("shouldContinue"), Function::New(
        isolate_, ShouldContinueCallback, v8::External::New(isolate_, this)));
    env_->Global()->Set(v8_str("Klass"), t->GetFunction());

    i::FLAG_turbo_osr = false;  // TODO(titzer): interrupts in TF loops.
    CompileRun("var obj = new Klass; while (obj.shouldContinue()) { }");
  }
};


class RequestInterruptTestWithAccessor
    : public RequestInterruptTestBaseWithSimpleInterrupt {
 public:
  virtual void TestBody() {
    v8::Local<v8::FunctionTemplate> t = v8::FunctionTemplate::New(isolate_);
    v8::Local<v8::Template> proto = t->PrototypeTemplate();
    proto->SetAccessorProperty(v8_str("shouldContinue"), FunctionTemplate::New(
        isolate_, ShouldContinueCallback, v8::External::New(isolate_, this)));
    env_->Global()->Set(v8_str("Klass"), t->GetFunction());

    i::FLAG_turbo_osr = false;  // TODO(titzer): interrupts in TF loops.
    CompileRun("var obj = new Klass; while (obj.shouldContinue) { }");
  }
};


class RequestInterruptTestWithNativeAccessor
    : public RequestInterruptTestBaseWithSimpleInterrupt {
 public:
  virtual void TestBody() {
    v8::Local<v8::FunctionTemplate> t = v8::FunctionTemplate::New(isolate_);
    t->InstanceTemplate()->SetNativeDataProperty(
        v8_str("shouldContinue"),
        &ShouldContinueNativeGetter,
        NULL,
        v8::External::New(isolate_, this));
    env_->Global()->Set(v8_str("Klass"), t->GetFunction());

    i::FLAG_turbo_osr = false;  // TODO(titzer): interrupts in TF loops.
    CompileRun("var obj = new Klass; while (obj.shouldContinue) { }");
  }

 private:
  static void ShouldContinueNativeGetter(
      Local<String> property,
      const v8::PropertyCallbackInfo<v8::Value>& info) {
    RequestInterruptTestBase* test =
        reinterpret_cast<RequestInterruptTestBase*>(
            info.Data().As<v8::External>()->Value());
    info.GetReturnValue().Set(test->ShouldContinue());
  }
};


class RequestInterruptTestWithMethodCallAndInterceptor
    : public RequestInterruptTestBaseWithSimpleInterrupt {
 public:
  virtual void TestBody() {
    v8::Local<v8::FunctionTemplate> t = v8::FunctionTemplate::New(isolate_);
    v8::Local<v8::Template> proto = t->PrototypeTemplate();
    proto->Set(v8_str("shouldContinue"), Function::New(
        isolate_, ShouldContinueCallback, v8::External::New(isolate_, this)));
    v8::Local<v8::ObjectTemplate> instance_template = t->InstanceTemplate();
    instance_template->SetHandler(
        v8::NamedPropertyHandlerConfiguration(EmptyInterceptor));

    env_->Global()->Set(v8_str("Klass"), t->GetFunction());

    i::FLAG_turbo_osr = false;  // TODO(titzer): interrupts in TF loops.
    CompileRun("var obj = new Klass; while (obj.shouldContinue()) { }");
  }

 private:
  static void EmptyInterceptor(
      Local<Name> property, const v8::PropertyCallbackInfo<v8::Value>& info) {}
};


class RequestInterruptTestWithMathAbs
    : public RequestInterruptTestBaseWithSimpleInterrupt {
 public:
  virtual void TestBody() {
    env_->Global()->Set(v8_str("WakeUpInterruptor"), Function::New(
        isolate_,
        WakeUpInterruptorCallback,
        v8::External::New(isolate_, this)));

    env_->Global()->Set(v8_str("ShouldContinue"), Function::New(
        isolate_,
        ShouldContinueCallback,
        v8::External::New(isolate_, this)));

    i::FLAG_allow_natives_syntax = true;
    i::FLAG_turbo_osr = false;  // TODO(titzer): interrupts in TF loops.
    CompileRun("function loopish(o) {"
               "  var pre = 10;"
               "  while (o.abs(1) > 0) {"
               "    if (o.abs(1) >= 0 && !ShouldContinue()) break;"
               "    if (pre > 0) {"
               "      if (--pre === 0) WakeUpInterruptor(o === Math);"
               "    }"
               "  }"
               "}"
               "var i = 50;"
               "var obj = {abs: function () { return i-- }, x: null};"
               "delete obj.x;"
               "loopish(obj);"
               "%OptimizeFunctionOnNextCall(loopish);"
               "loopish(Math);");

    i::FLAG_allow_natives_syntax = false;
  }

 private:
  static void WakeUpInterruptorCallback(
      const v8::FunctionCallbackInfo<Value>& info) {
    if (!info[0]->BooleanValue()) return;

    RequestInterruptTestBase* test =
        reinterpret_cast<RequestInterruptTestBase*>(
            info.Data().As<v8::External>()->Value());
    test->WakeUpInterruptor();
  }

  static void ShouldContinueCallback(
      const v8::FunctionCallbackInfo<Value>& info) {
    RequestInterruptTestBase* test =
        reinterpret_cast<RequestInterruptTestBase*>(
            info.Data().As<v8::External>()->Value());
    info.GetReturnValue().Set(test->should_continue());
  }
};


TEST(RequestInterruptTestWithFunctionCall) {
  RequestInterruptTestWithFunctionCall().RunTest();
}


TEST(RequestInterruptTestWithMethodCall) {
  RequestInterruptTestWithMethodCall().RunTest();
}


TEST(RequestInterruptTestWithAccessor) {
  RequestInterruptTestWithAccessor().RunTest();
}


TEST(RequestInterruptTestWithNativeAccessor) {
  RequestInterruptTestWithNativeAccessor().RunTest();
}


TEST(RequestInterruptTestWithMethodCallAndInterceptor) {
  RequestInterruptTestWithMethodCallAndInterceptor().RunTest();
}


TEST(RequestInterruptTestWithMathAbs) {
  RequestInterruptTestWithMathAbs().RunTest();
}


class RequestMultipleInterrupts : public RequestInterruptTestBase {
 public:
  RequestMultipleInterrupts() : i_thread(this), counter_(0) {}

  virtual void StartInterruptThread() {
    i_thread.Start();
  }

  virtual void TestBody() {
    Local<Function> func = Function::New(
        isolate_, ShouldContinueCallback, v8::External::New(isolate_, this));
    env_->Global()->Set(v8_str("ShouldContinue"), func);

    i::FLAG_turbo_osr = false;  // TODO(titzer): interrupts in TF loops.
    CompileRun("while (ShouldContinue()) { }");
  }

 private:
  class InterruptThread : public v8::base::Thread {
   public:
    enum { NUM_INTERRUPTS = 10 };
    explicit InterruptThread(RequestMultipleInterrupts* test)
        : Thread(Options("RequestInterruptTest")), test_(test) {}

    virtual void Run() {
      test_->sem_.Wait();
      for (int i = 0; i < NUM_INTERRUPTS; i++) {
        test_->isolate_->RequestInterrupt(&OnInterrupt, test_);
      }
    }

    static void OnInterrupt(v8::Isolate* isolate, void* data) {
      RequestMultipleInterrupts* test =
          reinterpret_cast<RequestMultipleInterrupts*>(data);
      test->should_continue_ = ++test->counter_ < NUM_INTERRUPTS;
    }

   private:
    RequestMultipleInterrupts* test_;
  };

  InterruptThread i_thread;
  int counter_;
};


TEST(RequestMultipleInterrupts) { RequestMultipleInterrupts().RunTest(); }


static Local<Value> function_new_expected_env;
static void FunctionNewCallback(const v8::FunctionCallbackInfo<Value>& info) {
  CHECK(function_new_expected_env->Equals(info.Data()));
  info.GetReturnValue().Set(17);
}


THREADED_TEST(FunctionNew) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);
  Local<Object> data = v8::Object::New(isolate);
  function_new_expected_env = data;
  Local<Function> func = Function::New(isolate, FunctionNewCallback, data);
  env->Global()->Set(v8_str("func"), func);
  Local<Value> result = CompileRun("func();");
  CHECK(v8::Integer::New(isolate, 17)->Equals(result));
  i::Isolate* i_isolate = reinterpret_cast<i::Isolate*>(isolate);
  // Verify function not cached
  auto serial_number = handle(i::Smi::cast(v8::Utils::OpenHandle(*func)
                                               ->shared()
                                               ->get_api_func_data()
                                               ->serial_number()),
                              i_isolate);
  auto cache = i_isolate->function_cache();
  CHECK(cache->Lookup(serial_number)->IsTheHole());
  // Verify that each Function::New creates a new function instance
  Local<Object> data2 = v8::Object::New(isolate);
  function_new_expected_env = data2;
  Local<Function> func2 = Function::New(isolate, FunctionNewCallback, data2);
  CHECK(!func2->IsNull());
  CHECK(!func->Equals(func2));
  env->Global()->Set(v8_str("func2"), func2);
  Local<Value> result2 = CompileRun("func2();");
  CHECK(v8::Integer::New(isolate, 17)->Equals(result2));
}


TEST(EscapeableHandleScope) {
  HandleScope outer_scope(CcTest::isolate());
  LocalContext context;
  const int runs = 10;
  Local<String> values[runs];
  for (int i = 0; i < runs; i++) {
    v8::EscapableHandleScope inner_scope(CcTest::isolate());
    Local<String> value;
    if (i != 0) value = v8_str("escape value");
    values[i] = inner_scope.Escape(value);
  }
  for (int i = 0; i < runs; i++) {
    Local<String> expected;
    if (i != 0) {
      CHECK(v8_str("escape value")->Equals(values[i]));
    } else {
      CHECK(values[i].IsEmpty());
    }
  }
}


static void SetterWhichExpectsThisAndHolderToDiffer(
    Local<String>, Local<Value>, const v8::PropertyCallbackInfo<void>& info) {
  CHECK(info.Holder() != info.This());
}


TEST(Regress239669) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);
  Local<ObjectTemplate> templ = ObjectTemplate::New(isolate);
  templ->SetAccessor(v8_str("x"), 0, SetterWhichExpectsThisAndHolderToDiffer);
  context->Global()->Set(v8_str("P"), templ->NewInstance());
  CompileRun(
      "function C1() {"
      "  this.x = 23;"
      "};"
      "C1.prototype = P;"
      "for (var i = 0; i < 4; i++ ) {"
      "  new C1();"
      "}");
}


class ApiCallOptimizationChecker {
 private:
  static Local<Object> data;
  static Local<Object> receiver;
  static Local<Object> holder;
  static Local<Object> callee;
  static int count;

  static void OptimizationCallback(
      const v8::FunctionCallbackInfo<v8::Value>& info) {
    CHECK(callee == info.Callee());
    CHECK(data == info.Data());
    CHECK(receiver == info.This());
    if (info.Length() == 1) {
      CHECK(v8_num(1)->Equals(info[0]));
    }
    CHECK(holder == info.Holder());
    count++;
    info.GetReturnValue().Set(v8_str("returned"));
  }

 public:
  enum SignatureType {
    kNoSignature,
    kSignatureOnReceiver,
    kSignatureOnPrototype
  };

  void RunAll() {
    SignatureType signature_types[] =
      {kNoSignature, kSignatureOnReceiver, kSignatureOnPrototype};
    for (unsigned i = 0; i < arraysize(signature_types); i++) {
      SignatureType signature_type = signature_types[i];
      for (int j = 0; j < 2; j++) {
        bool global = j == 0;
        int key = signature_type +
            arraysize(signature_types) * (global ? 1 : 0);
        Run(signature_type, global, key);
      }
    }
  }

  void Run(SignatureType signature_type, bool global, int key) {
    v8::Isolate* isolate = CcTest::isolate();
    v8::HandleScope scope(isolate);
    // Build a template for signature checks.
    Local<v8::ObjectTemplate> signature_template;
    Local<v8::Signature> signature;
    {
      Local<v8::FunctionTemplate> parent_template =
        FunctionTemplate::New(isolate);
      parent_template->SetHiddenPrototype(true);
      Local<v8::FunctionTemplate> function_template
          = FunctionTemplate::New(isolate);
      function_template->Inherit(parent_template);
      switch (signature_type) {
        case kNoSignature:
          break;
        case kSignatureOnReceiver:
          signature = v8::Signature::New(isolate, function_template);
          break;
        case kSignatureOnPrototype:
          signature = v8::Signature::New(isolate, parent_template);
          break;
      }
      signature_template = function_template->InstanceTemplate();
    }
    // Global object must pass checks.
    Local<v8::Context> context =
        v8::Context::New(isolate, NULL, signature_template);
    v8::Context::Scope context_scope(context);
    // Install regular object that can pass signature checks.
    Local<Object> function_receiver = signature_template->NewInstance();
    context->Global()->Set(v8_str("function_receiver"), function_receiver);
    // Get the holder objects.
    Local<Object> inner_global =
        Local<Object>::Cast(context->Global()->GetPrototype());
    // Install functions on hidden prototype object if there is one.
    data = Object::New(isolate);
    Local<FunctionTemplate> function_template = FunctionTemplate::New(
        isolate, OptimizationCallback, data, signature);
    Local<Function> function = function_template->GetFunction();
    Local<Object> global_holder = inner_global;
    Local<Object> function_holder = function_receiver;
    if (signature_type == kSignatureOnPrototype) {
      function_holder = Local<Object>::Cast(function_holder->GetPrototype());
      global_holder = Local<Object>::Cast(global_holder->GetPrototype());
    }
    global_holder->Set(v8_str("g_f"), function);
    global_holder->SetAccessorProperty(v8_str("g_acc"), function, function);
    function_holder->Set(v8_str("f"), function);
    function_holder->SetAccessorProperty(v8_str("acc"), function, function);
    // Initialize expected values.
    callee = function;
    count = 0;
    if (global) {
      receiver = context->Global();
      holder = inner_global;
    } else {
      holder = function_receiver;
      // If not using a signature, add something else to the prototype chain
      // to test the case that holder != receiver
      if (signature_type == kNoSignature) {
        receiver = Local<Object>::Cast(CompileRun(
            "var receiver_subclass = {};\n"
            "receiver_subclass.__proto__ = function_receiver;\n"
            "receiver_subclass"));
      } else {
        receiver = Local<Object>::Cast(CompileRun(
          "var receiver_subclass = function_receiver;\n"
          "receiver_subclass"));
      }
    }
    // With no signature, the holder is not set.
    if (signature_type == kNoSignature) holder = receiver;
    // build wrap_function
    i::ScopedVector<char> wrap_function(200);
    if (global) {
      i::SNPrintF(
          wrap_function,
          "function wrap_f_%d() { var f = g_f; return f(); }\n"
          "function wrap_get_%d() { return this.g_acc; }\n"
          "function wrap_set_%d() { return this.g_acc = 1; }\n",
          key, key, key);
    } else {
      i::SNPrintF(
          wrap_function,
          "function wrap_f_%d() { return receiver_subclass.f(); }\n"
          "function wrap_get_%d() { return receiver_subclass.acc; }\n"
          "function wrap_set_%d() { return receiver_subclass.acc = 1; }\n",
          key, key, key);
    }
    // build source string
    i::ScopedVector<char> source(1000);
    i::SNPrintF(
        source,
        "%s\n"  // wrap functions
        "function wrap_f() { return wrap_f_%d(); }\n"
        "function wrap_get() { return wrap_get_%d(); }\n"
        "function wrap_set() { return wrap_set_%d(); }\n"
        "check = function(returned) {\n"
        "  if (returned !== 'returned') { throw returned; }\n"
        "}\n"
        "\n"
        "check(wrap_f());\n"
        "check(wrap_f());\n"
        "%%OptimizeFunctionOnNextCall(wrap_f_%d);\n"
        "check(wrap_f());\n"
        "\n"
        "check(wrap_get());\n"
        "check(wrap_get());\n"
        "%%OptimizeFunctionOnNextCall(wrap_get_%d);\n"
        "check(wrap_get());\n"
        "\n"
        "check = function(returned) {\n"
        "  if (returned !== 1) { throw returned; }\n"
        "}\n"
        "check(wrap_set());\n"
        "check(wrap_set());\n"
        "%%OptimizeFunctionOnNextCall(wrap_set_%d);\n"
        "check(wrap_set());\n",
        wrap_function.start(), key, key, key, key, key, key);
    v8::TryCatch try_catch;
    CompileRun(source.start());
    DCHECK(!try_catch.HasCaught());
    CHECK_EQ(9, count);
  }
};


Local<Object> ApiCallOptimizationChecker::data;
Local<Object> ApiCallOptimizationChecker::receiver;
Local<Object> ApiCallOptimizationChecker::holder;
Local<Object> ApiCallOptimizationChecker::callee;
int ApiCallOptimizationChecker::count = 0;


TEST(FunctionCallOptimization) {
  i::FLAG_allow_natives_syntax = true;
  ApiCallOptimizationChecker checker;
  checker.RunAll();
}


TEST(FunctionCallOptimizationMultipleArgs) {
  i::FLAG_allow_natives_syntax = true;
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);
  Handle<Object> global = context->Global();
  Local<v8::Function> function = Function::New(isolate, Returns42);
  global->Set(v8_str("x"), function);
  CompileRun(
      "function x_wrap() {\n"
      "  for (var i = 0; i < 5; i++) {\n"
      "    x(1,2,3);\n"
      "  }\n"
      "}\n"
      "x_wrap();\n"
      "%OptimizeFunctionOnNextCall(x_wrap);"
      "x_wrap();\n");
}


static void ReturnsSymbolCallback(
    const v8::FunctionCallbackInfo<v8::Value>& info) {
  info.GetReturnValue().Set(v8::Symbol::New(info.GetIsolate()));
}


TEST(ApiCallbackCanReturnSymbols) {
  i::FLAG_allow_natives_syntax = true;
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);
  Handle<Object> global = context->Global();
  Local<v8::Function> function = Function::New(isolate, ReturnsSymbolCallback);
  global->Set(v8_str("x"), function);
  CompileRun(
      "function x_wrap() {\n"
      "  for (var i = 0; i < 5; i++) {\n"
      "    x();\n"
      "  }\n"
      "}\n"
      "x_wrap();\n"
      "%OptimizeFunctionOnNextCall(x_wrap);"
      "x_wrap();\n");
}


TEST(EmptyApiCallback) {
  LocalContext context;
  auto isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);
  auto global = context->Global();
  auto function = FunctionTemplate::New(isolate)->GetFunction();
  global->Set(v8_str("x"), function);

  auto result = CompileRun("x()");
  CHECK(v8::Utils::OpenHandle(*result)->IsJSGlobalProxy());

  result = CompileRun("x(1,2,3)");
  CHECK(v8::Utils::OpenHandle(*result)->IsJSGlobalProxy());

  result = CompileRun("7 + x.call(3) + 11");
  CHECK(result->IsInt32());
  CHECK_EQ(21, result->Int32Value());

  result = CompileRun("7 + x.call(3, 101, 102, 103, 104) + 11");
  CHECK(result->IsInt32());
  CHECK_EQ(21, result->Int32Value());

  result = CompileRun("var y = []; x.call(y)");
  CHECK(result->IsArray());

  result = CompileRun("x.call(y, 1, 2, 3, 4)");
  CHECK(result->IsArray());
}


TEST(SimpleSignatureCheck) {
  LocalContext context;
  auto isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);
  auto global = context->Global();
  auto sig_obj = FunctionTemplate::New(isolate);
  auto sig = v8::Signature::New(isolate, sig_obj);
  auto x = FunctionTemplate::New(isolate, Returns42, Handle<Value>(), sig);
  global->Set(v8_str("sig_obj"), sig_obj->GetFunction());
  global->Set(v8_str("x"), x->GetFunction());
  CompileRun("var s = new sig_obj();");
  {
    TryCatch try_catch(isolate);
    CompileRun("x()");
    CHECK(try_catch.HasCaught());
  }
  {
    TryCatch try_catch(isolate);
    CompileRun("x.call(1)");
    CHECK(try_catch.HasCaught());
  }
  {
    TryCatch try_catch(isolate);
    auto result = CompileRun("s.x = x; s.x()");
    CHECK(!try_catch.HasCaught());
    CHECK_EQ(42, result->Int32Value());
  }
  {
    TryCatch try_catch(isolate);
    auto result = CompileRun("x.call(s)");
    CHECK(!try_catch.HasCaught());
    CHECK_EQ(42, result->Int32Value());
  }
}


TEST(ChainSignatureCheck) {
  LocalContext context;
  auto isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);
  auto global = context->Global();
  auto sig_obj = FunctionTemplate::New(isolate);
  auto sig = v8::Signature::New(isolate, sig_obj);
  for (int i = 0; i < 4; ++i) {
    auto temp = FunctionTemplate::New(isolate);
    temp->Inherit(sig_obj);
    sig_obj = temp;
  }
  auto x = FunctionTemplate::New(isolate, Returns42, Handle<Value>(), sig);
  global->Set(v8_str("sig_obj"), sig_obj->GetFunction());
  global->Set(v8_str("x"), x->GetFunction());
  CompileRun("var s = new sig_obj();");
  {
    TryCatch try_catch(isolate);
    CompileRun("x()");
    CHECK(try_catch.HasCaught());
  }
  {
    TryCatch try_catch(isolate);
    CompileRun("x.call(1)");
    CHECK(try_catch.HasCaught());
  }
  {
    TryCatch try_catch(isolate);
    auto result = CompileRun("s.x = x; s.x()");
    CHECK(!try_catch.HasCaught());
    CHECK_EQ(42, result->Int32Value());
  }
  {
    TryCatch try_catch(isolate);
    auto result = CompileRun("x.call(s)");
    CHECK(!try_catch.HasCaught());
    CHECK_EQ(42, result->Int32Value());
  }
}


TEST(PrototypeSignatureCheck) {
  LocalContext context;
  auto isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);
  auto global = context->Global();
  auto sig_obj = FunctionTemplate::New(isolate);
  sig_obj->SetHiddenPrototype(true);
  auto sig = v8::Signature::New(isolate, sig_obj);
  auto x = FunctionTemplate::New(isolate, Returns42, Handle<Value>(), sig);
  global->Set(v8_str("sig_obj"), sig_obj->GetFunction());
  global->Set(v8_str("x"), x->GetFunction());
  CompileRun("s = {}; s.__proto__ = new sig_obj();");
  {
    TryCatch try_catch(isolate);
    CompileRun("x()");
    CHECK(try_catch.HasCaught());
  }
  {
    TryCatch try_catch(isolate);
    CompileRun("x.call(1)");
    CHECK(try_catch.HasCaught());
  }
  {
    TryCatch try_catch(isolate);
    auto result = CompileRun("s.x = x; s.x()");
    CHECK(!try_catch.HasCaught());
    CHECK_EQ(42, result->Int32Value());
  }
  {
    TryCatch try_catch(isolate);
    auto result = CompileRun("x.call(s)");
    CHECK(!try_catch.HasCaught());
    CHECK_EQ(42, result->Int32Value());
  }
}


static const char* last_event_message;
static int last_event_status;
void StoringEventLoggerCallback(const char* message, int status) {
    last_event_message = message;
    last_event_status = status;
}


TEST(EventLogging) {
  v8::Isolate* isolate = CcTest::isolate();
  isolate->SetEventLogger(StoringEventLoggerCallback);
  v8::internal::HistogramTimer histogramTimer(
      "V8.Test", 0, 10000, v8::internal::HistogramTimer::MILLISECOND, 50,
      reinterpret_cast<v8::internal::Isolate*>(isolate));
  histogramTimer.Start();
  CHECK_EQ(0, strcmp("V8.Test", last_event_message));
  CHECK_EQ(0, last_event_status);
  histogramTimer.Stop();
  CHECK_EQ(0, strcmp("V8.Test", last_event_message));
  CHECK_EQ(1, last_event_status);
}


TEST(Promises) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);
  Handle<Object> global = context->Global();

  // Creation.
  Handle<v8::Promise::Resolver> pr = v8::Promise::Resolver::New(isolate);
  Handle<v8::Promise::Resolver> rr = v8::Promise::Resolver::New(isolate);
  Handle<v8::Promise> p = pr->GetPromise();
  Handle<v8::Promise> r = rr->GetPromise();
  CHECK_EQ(isolate, p->GetIsolate());

  // IsPromise predicate.
  CHECK(p->IsPromise());
  CHECK(r->IsPromise());
  Handle<Value> o = v8::Object::New(isolate);
  CHECK(!o->IsPromise());

  // Resolution and rejection.
  pr->Resolve(v8::Integer::New(isolate, 1));
  CHECK(p->IsPromise());
  rr->Reject(v8::Integer::New(isolate, 2));
  CHECK(r->IsPromise());

  // Chaining non-pending promises.
  CompileRun(
      "var x1 = 0;\n"
      "var x2 = 0;\n"
      "function f1(x) { x1 = x; return x+1 };\n"
      "function f2(x) { x2 = x; return x+1 };\n");
  Handle<Function> f1 = Handle<Function>::Cast(global->Get(v8_str("f1")));
  Handle<Function> f2 = Handle<Function>::Cast(global->Get(v8_str("f2")));

  p->Chain(f1);
  CHECK_EQ(0, global->Get(v8_str("x1"))->Int32Value());
  isolate->RunMicrotasks();
  CHECK_EQ(1, global->Get(v8_str("x1"))->Int32Value());

  p->Catch(f2);
  isolate->RunMicrotasks();
  CHECK_EQ(0, global->Get(v8_str("x2"))->Int32Value());

  r->Catch(f2);
  CHECK_EQ(0, global->Get(v8_str("x2"))->Int32Value());
  isolate->RunMicrotasks();
  CHECK_EQ(2, global->Get(v8_str("x2"))->Int32Value());

  r->Chain(f1);
  isolate->RunMicrotasks();
  CHECK_EQ(1, global->Get(v8_str("x1"))->Int32Value());

  // Chaining pending promises.
  CompileRun("x1 = x2 = 0;");
  pr = v8::Promise::Resolver::New(isolate);
  rr = v8::Promise::Resolver::New(isolate);

  pr->GetPromise()->Chain(f1);
  rr->GetPromise()->Catch(f2);
  isolate->RunMicrotasks();
  CHECK_EQ(0, global->Get(v8_str("x1"))->Int32Value());
  CHECK_EQ(0, global->Get(v8_str("x2"))->Int32Value());

  pr->Resolve(v8::Integer::New(isolate, 1));
  rr->Reject(v8::Integer::New(isolate, 2));
  CHECK_EQ(0, global->Get(v8_str("x1"))->Int32Value());
  CHECK_EQ(0, global->Get(v8_str("x2"))->Int32Value());

  isolate->RunMicrotasks();
  CHECK_EQ(1, global->Get(v8_str("x1"))->Int32Value());
  CHECK_EQ(2, global->Get(v8_str("x2"))->Int32Value());

  // Multi-chaining.
  CompileRun("x1 = x2 = 0;");
  pr = v8::Promise::Resolver::New(isolate);
  pr->GetPromise()->Chain(f1)->Chain(f2);
  pr->Resolve(v8::Integer::New(isolate, 3));
  CHECK_EQ(0, global->Get(v8_str("x1"))->Int32Value());
  CHECK_EQ(0, global->Get(v8_str("x2"))->Int32Value());
  isolate->RunMicrotasks();
  CHECK_EQ(3, global->Get(v8_str("x1"))->Int32Value());
  CHECK_EQ(4, global->Get(v8_str("x2"))->Int32Value());

  CompileRun("x1 = x2 = 0;");
  rr = v8::Promise::Resolver::New(isolate);
  rr->GetPromise()->Catch(f1)->Chain(f2);
  rr->Reject(v8::Integer::New(isolate, 3));
  CHECK_EQ(0, global->Get(v8_str("x1"))->Int32Value());
  CHECK_EQ(0, global->Get(v8_str("x2"))->Int32Value());
  isolate->RunMicrotasks();
  CHECK_EQ(3, global->Get(v8_str("x1"))->Int32Value());
  CHECK_EQ(4, global->Get(v8_str("x2"))->Int32Value());
}


TEST(PromiseThen) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);
  Handle<Object> global = context->Global();

  // Creation.
  Handle<v8::Promise::Resolver> pr = v8::Promise::Resolver::New(isolate);
  Handle<v8::Promise::Resolver> qr = v8::Promise::Resolver::New(isolate);
  Handle<v8::Promise> p = pr->GetPromise();
  Handle<v8::Promise> q = qr->GetPromise();

  CHECK(p->IsPromise());
  CHECK(q->IsPromise());

  pr->Resolve(v8::Integer::New(isolate, 1));
  qr->Resolve(p);

  // Chaining non-pending promises.
  CompileRun(
      "var x1 = 0;\n"
      "var x2 = 0;\n"
      "function f1(x) { x1 = x; return x+1 };\n"
      "function f2(x) { x2 = x; return x+1 };\n");
  Handle<Function> f1 = Handle<Function>::Cast(global->Get(v8_str("f1")));
  Handle<Function> f2 = Handle<Function>::Cast(global->Get(v8_str("f2")));

  // Chain
  q->Chain(f1);
  CHECK(global->Get(v8_str("x1"))->IsNumber());
  CHECK_EQ(0, global->Get(v8_str("x1"))->Int32Value());
  isolate->RunMicrotasks();
  CHECK(!global->Get(v8_str("x1"))->IsNumber());
  CHECK(p->Equals(global->Get(v8_str("x1"))));

  // Then
  CompileRun("x1 = x2 = 0;");
  q->Then(f1);
  CHECK_EQ(0, global->Get(v8_str("x1"))->Int32Value());
  isolate->RunMicrotasks();
  CHECK_EQ(1, global->Get(v8_str("x1"))->Int32Value());

  // Then
  CompileRun("x1 = x2 = 0;");
  pr = v8::Promise::Resolver::New(isolate);
  qr = v8::Promise::Resolver::New(isolate);

  qr->Resolve(pr);
  qr->GetPromise()->Then(f1)->Then(f2);

  CHECK_EQ(0, global->Get(v8_str("x1"))->Int32Value());
  CHECK_EQ(0, global->Get(v8_str("x2"))->Int32Value());
  isolate->RunMicrotasks();
  CHECK_EQ(0, global->Get(v8_str("x1"))->Int32Value());
  CHECK_EQ(0, global->Get(v8_str("x2"))->Int32Value());

  pr->Resolve(v8::Integer::New(isolate, 3));

  CHECK_EQ(0, global->Get(v8_str("x1"))->Int32Value());
  CHECK_EQ(0, global->Get(v8_str("x2"))->Int32Value());
  isolate->RunMicrotasks();
  CHECK_EQ(3, global->Get(v8_str("x1"))->Int32Value());
  CHECK_EQ(4, global->Get(v8_str("x2"))->Int32Value());
}


TEST(DisallowJavascriptExecutionScope) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);
  v8::Isolate::DisallowJavascriptExecutionScope no_js(
      isolate, v8::Isolate::DisallowJavascriptExecutionScope::CRASH_ON_FAILURE);
  CompileRun("2+2");
}


TEST(AllowJavascriptExecutionScope) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);
  v8::Isolate::DisallowJavascriptExecutionScope no_js(
      isolate, v8::Isolate::DisallowJavascriptExecutionScope::CRASH_ON_FAILURE);
  v8::Isolate::DisallowJavascriptExecutionScope throw_js(
      isolate, v8::Isolate::DisallowJavascriptExecutionScope::THROW_ON_FAILURE);
  { v8::Isolate::AllowJavascriptExecutionScope yes_js(isolate);
    CompileRun("1+1");
  }
}


TEST(ThrowOnJavascriptExecution) {
  LocalContext context;
  v8::Isolate* isolate = context->GetIsolate();
  v8::HandleScope scope(isolate);
  v8::TryCatch try_catch;
  v8::Isolate::DisallowJavascriptExecutionScope throw_js(
      isolate, v8::Isolate::DisallowJavascriptExecutionScope::THROW_ON_FAILURE);
  CompileRun("1+1");
  CHECK(try_catch.HasCaught());
}


TEST(Regress354123) {
  LocalContext current;
  v8::Isolate* isolate = current->GetIsolate();
  v8::HandleScope scope(isolate);

  v8::Handle<v8::ObjectTemplate> templ = v8::ObjectTemplate::New(isolate);
  templ->SetAccessCheckCallbacks(AccessCounter, NULL);
  current->Global()->Set(v8_str("friend"), templ->NewInstance());

  // Test access using __proto__ from the prototype chain.
  access_count = 0;
  CompileRun("friend.__proto__ = {};");
  CHECK_EQ(2, access_count);
  CompileRun("friend.__proto__;");
  CHECK_EQ(4, access_count);

  // Test access using __proto__ as a hijacked function (A).
  access_count = 0;
  CompileRun("var p = Object.prototype;"
             "var f = Object.getOwnPropertyDescriptor(p, '__proto__').set;"
             "f.call(friend, {});");
  CHECK_EQ(1, access_count);
  CompileRun("var p = Object.prototype;"
             "var f = Object.getOwnPropertyDescriptor(p, '__proto__').get;"
             "f.call(friend);");
  CHECK_EQ(2, access_count);

  // Test access using __proto__ as a hijacked function (B).
  access_count = 0;
  CompileRun("var f = Object.prototype.__lookupSetter__('__proto__');"
             "f.call(friend, {});");
  CHECK_EQ(1, access_count);
  CompileRun("var f = Object.prototype.__lookupGetter__('__proto__');"
             "f.call(friend);");
  CHECK_EQ(2, access_count);

  // Test access using Object.setPrototypeOf reflective method.
  access_count = 0;
  CompileRun("Object.setPrototypeOf(friend, {});");
  CHECK_EQ(1, access_count);
  CompileRun("Object.getPrototypeOf(friend);");
  CHECK_EQ(2, access_count);
}


TEST(CaptureStackTraceForStackOverflow) {
  v8::internal::FLAG_stack_size = 150;
  LocalContext current;
  v8::Isolate* isolate = current->GetIsolate();
  v8::HandleScope scope(isolate);
  V8::SetCaptureStackTraceForUncaughtExceptions(
      true, 10, v8::StackTrace::kDetailed);
  v8::TryCatch try_catch;
  CompileRun("(function f(x) { f(x+1); })(0)");
  CHECK(try_catch.HasCaught());
}


TEST(ScriptNameAndLineNumber) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);
  const char* url = "http://www.foo.com/foo.js";
  v8::ScriptOrigin origin(v8_str(url), v8::Integer::New(isolate, 13));
  v8::ScriptCompiler::Source script_source(v8_str("var foo;"), origin);
  Local<Script> script = v8::ScriptCompiler::Compile(
      isolate, &script_source);
  Local<Value> script_name = script->GetUnboundScript()->GetScriptName();
  CHECK(!script_name.IsEmpty());
  CHECK(script_name->IsString());
  String::Utf8Value utf8_name(script_name);
  CHECK_EQ(0, strcmp(url, *utf8_name));
  int line_number = script->GetUnboundScript()->GetLineNumber(0);
  CHECK_EQ(13, line_number);
}

void CheckMagicComments(Handle<Script> script, const char* expected_source_url,
                        const char* expected_source_mapping_url) {
  if (expected_source_url != NULL) {
    v8::String::Utf8Value url(script->GetUnboundScript()->GetSourceURL());
    CHECK_EQ(0, strcmp(expected_source_url, *url));
  } else {
    CHECK(script->GetUnboundScript()->GetSourceURL()->IsUndefined());
  }
  if (expected_source_mapping_url != NULL) {
    v8::String::Utf8Value url(
        script->GetUnboundScript()->GetSourceMappingURL());
    CHECK_EQ(0, strcmp(expected_source_mapping_url, *url));
  } else {
    CHECK(script->GetUnboundScript()->GetSourceMappingURL()->IsUndefined());
  }
}

void SourceURLHelper(const char* source, const char* expected_source_url,
                     const char* expected_source_mapping_url) {
  Local<Script> script = v8_compile(source);
  CheckMagicComments(script, expected_source_url, expected_source_mapping_url);
}


TEST(ScriptSourceURLAndSourceMappingURL) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);
  SourceURLHelper("function foo() {}\n"
                  "//# sourceURL=bar1.js\n", "bar1.js", NULL);
  SourceURLHelper("function foo() {}\n"
                  "//# sourceMappingURL=bar2.js\n", NULL, "bar2.js");

  // Both sourceURL and sourceMappingURL.
  SourceURLHelper("function foo() {}\n"
                  "//# sourceURL=bar3.js\n"
                  "//# sourceMappingURL=bar4.js\n", "bar3.js", "bar4.js");

  // Two source URLs; the first one is ignored.
  SourceURLHelper("function foo() {}\n"
                  "//# sourceURL=ignoreme.js\n"
                  "//# sourceURL=bar5.js\n", "bar5.js", NULL);
  SourceURLHelper("function foo() {}\n"
                  "//# sourceMappingURL=ignoreme.js\n"
                  "//# sourceMappingURL=bar6.js\n", NULL, "bar6.js");

  // SourceURL or sourceMappingURL in the middle of the script.
  SourceURLHelper("function foo() {}\n"
                  "//# sourceURL=bar7.js\n"
                  "function baz() {}\n", "bar7.js", NULL);
  SourceURLHelper("function foo() {}\n"
                  "//# sourceMappingURL=bar8.js\n"
                  "function baz() {}\n", NULL, "bar8.js");

  // Too much whitespace.
  SourceURLHelper("function foo() {}\n"
                  "//#  sourceURL=bar9.js\n"
                  "//#  sourceMappingURL=bar10.js\n", NULL, NULL);
  SourceURLHelper("function foo() {}\n"
                  "//# sourceURL =bar11.js\n"
                  "//# sourceMappingURL =bar12.js\n", NULL, NULL);

  // Disallowed characters in value.
  SourceURLHelper("function foo() {}\n"
                  "//# sourceURL=bar13 .js   \n"
                  "//# sourceMappingURL=bar14 .js \n",
                  NULL, NULL);
  SourceURLHelper("function foo() {}\n"
                  "//# sourceURL=bar15\t.js   \n"
                  "//# sourceMappingURL=bar16\t.js \n",
                  NULL, NULL);
  SourceURLHelper("function foo() {}\n"
                  "//# sourceURL=bar17'.js   \n"
                  "//# sourceMappingURL=bar18'.js \n",
                  NULL, NULL);
  SourceURLHelper("function foo() {}\n"
                  "//# sourceURL=bar19\".js   \n"
                  "//# sourceMappingURL=bar20\".js \n",
                  NULL, NULL);

  // Not too much whitespace.
  SourceURLHelper("function foo() {}\n"
                  "//# sourceURL=  bar21.js   \n"
                  "//# sourceMappingURL=  bar22.js \n", "bar21.js", "bar22.js");
}


TEST(GetOwnPropertyDescriptor) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);
  CompileRun(
    "var x = { value : 13};"
    "Object.defineProperty(x, 'p0', {value : 12});"
    "Object.defineProperty(x, 'p1', {"
    "  set : function(value) { this.value = value; },"
    "  get : function() { return this.value; },"
    "});");
  Local<Object> x = Local<Object>::Cast(env->Global()->Get(v8_str("x")));
  Local<Value> desc = x->GetOwnPropertyDescriptor(v8_str("no_prop"));
  CHECK(desc->IsUndefined());
  desc = x->GetOwnPropertyDescriptor(v8_str("p0"));
  CHECK(v8_num(12)->Equals(Local<Object>::Cast(desc)->Get(v8_str("value"))));
  desc = x->GetOwnPropertyDescriptor(v8_str("p1"));
  Local<Function> set =
    Local<Function>::Cast(Local<Object>::Cast(desc)->Get(v8_str("set")));
  Local<Function> get =
    Local<Function>::Cast(Local<Object>::Cast(desc)->Get(v8_str("get")));
  CHECK(v8_num(13)->Equals(get->Call(x, 0, NULL)));
  Handle<Value> args[] = { v8_num(14) };
  set->Call(x, 1, args);
  CHECK(v8_num(14)->Equals(get->Call(x, 0, NULL)));
}


TEST(Regress411877) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope handle_scope(isolate);
  v8::Handle<v8::ObjectTemplate> object_template =
      v8::ObjectTemplate::New(isolate);
  object_template->SetAccessCheckCallbacks(AccessCounter, NULL);

  v8::Handle<Context> context = Context::New(isolate);
  v8::Context::Scope context_scope(context);

  context->Global()->Set(v8_str("o"), object_template->NewInstance());
  CompileRun("Object.getOwnPropertyNames(o)");
}


TEST(GetHiddenPropertyTableAfterAccessCheck) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope handle_scope(isolate);
  v8::Handle<v8::ObjectTemplate> object_template =
      v8::ObjectTemplate::New(isolate);
  object_template->SetAccessCheckCallbacks(AccessCounter, NULL);

  v8::Handle<Context> context = Context::New(isolate);
  v8::Context::Scope context_scope(context);

  v8::Handle<v8::Object> obj = object_template->NewInstance();
  obj->Set(v8_str("key"), v8_str("value"));
  obj->Delete(v8_str("key"));

  obj->SetHiddenValue(v8_str("hidden key 2"), v8_str("hidden value 2"));
}


TEST(Regress411793) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope handle_scope(isolate);
  v8::Handle<v8::ObjectTemplate> object_template =
      v8::ObjectTemplate::New(isolate);
  object_template->SetAccessCheckCallbacks(AccessCounter, NULL);

  v8::Handle<Context> context = Context::New(isolate);
  v8::Context::Scope context_scope(context);

  context->Global()->Set(v8_str("o"), object_template->NewInstance());
  CompileRun(
      "Object.defineProperty(o, 'key', "
      "    { get: function() {}, set: function() {} });");
}

class TestSourceStream : public v8::ScriptCompiler::ExternalSourceStream {
 public:
  explicit TestSourceStream(const char** chunks) : chunks_(chunks), index_(0) {}

  virtual size_t GetMoreData(const uint8_t** src) {
    // Unlike in real use cases, this function will never block.
    if (chunks_[index_] == NULL) {
      return 0;
    }
    // Copy the data, since the caller takes ownership of it.
    size_t len = strlen(chunks_[index_]);
    // We don't need to zero-terminate since we return the length.
    uint8_t* copy = new uint8_t[len];
    memcpy(copy, chunks_[index_], len);
    *src = copy;
    ++index_;
    return len;
  }

  // Helper for constructing a string from chunks (the compilation needs it
  // too).
  static char* FullSourceString(const char** chunks) {
    size_t total_len = 0;
    for (size_t i = 0; chunks[i] != NULL; ++i) {
      total_len += strlen(chunks[i]);
    }
    char* full_string = new char[total_len + 1];
    size_t offset = 0;
    for (size_t i = 0; chunks[i] != NULL; ++i) {
      size_t len = strlen(chunks[i]);
      memcpy(full_string + offset, chunks[i], len);
      offset += len;
    }
    full_string[total_len] = 0;
    return full_string;
  }

 private:
  const char** chunks_;
  unsigned index_;
};


// Helper function for running streaming tests.
void RunStreamingTest(const char** chunks,
                      v8::ScriptCompiler::StreamedSource::Encoding encoding =
                          v8::ScriptCompiler::StreamedSource::ONE_BYTE,
                      bool expected_success = true,
                      const char* expected_source_url = NULL,
                      const char* expected_source_mapping_url = NULL) {
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);
  v8::TryCatch try_catch;

  v8::ScriptCompiler::StreamedSource source(new TestSourceStream(chunks),
                                            encoding);
  v8::ScriptCompiler::ScriptStreamingTask* task =
      v8::ScriptCompiler::StartStreamingScript(isolate, &source);

  // TestSourceStream::GetMoreData won't block, so it's OK to just run the
  // task here in the main thread.
  task->Run();
  delete task;

  // Possible errors are only produced while compiling.
  CHECK_EQ(false, try_catch.HasCaught());

  v8::ScriptOrigin origin(v8_str("http://foo.com"));
  char* full_source = TestSourceStream::FullSourceString(chunks);
  v8::Handle<Script> script = v8::ScriptCompiler::Compile(
      isolate, &source, v8_str(full_source), origin);
  if (expected_success) {
    CHECK(!script.IsEmpty());
    v8::Handle<Value> result(script->Run());
    // All scripts are supposed to return the fixed value 13 when ran.
    CHECK_EQ(13, result->Int32Value());
    CheckMagicComments(script, expected_source_url,
                       expected_source_mapping_url);
  } else {
    CHECK(script.IsEmpty());
    CHECK(try_catch.HasCaught());
  }
  delete[] full_source;
}


TEST(StreamingSimpleScript) {
  // This script is unrealistically small, since no one chunk is enough to fill
  // the backing buffer of Scanner, let alone overflow it.
  const char* chunks[] = {"function foo() { ret", "urn 13; } f", "oo(); ",
                          NULL};
  RunStreamingTest(chunks);
}


TEST(StreamingBiggerScript) {
  const char* chunk1 =
      "function foo() {\n"
      "  // Make this chunk sufficiently long so that it will overflow the\n"
      "  // backing buffer of the Scanner.\n"
      "  var i = 0;\n"
      "  var result = 0;\n"
      "  for (i = 0; i < 13; ++i) { result = result + 1; }\n"
      "  result = 0;\n"
      "  for (i = 0; i < 13; ++i) { result = result + 1; }\n"
      "  result = 0;\n"
      "  for (i = 0; i < 13; ++i) { result = result + 1; }\n"
      "  result = 0;\n"
      "  for (i = 0; i < 13; ++i) { result = result + 1; }\n"
      "  return result;\n"
      "}\n";
  const char* chunks[] = {chunk1, "foo(); ", NULL};
  RunStreamingTest(chunks);
}


TEST(StreamingScriptWithParseError) {
  // Test that parse errors from streamed scripts are propagated correctly.
  {
    char chunk1[] =
        "  // This will result in a parse error.\n"
        "  var if else then foo";
    char chunk2[] = "  13\n";
    const char* chunks[] = {chunk1, chunk2, "foo();", NULL};

    RunStreamingTest(chunks, v8::ScriptCompiler::StreamedSource::ONE_BYTE,
                     false);
  }
  // Test that the next script succeeds normally.
  {
    char chunk1[] =
        "  // This will be parsed successfully.\n"
        "  function foo() { return ";
    char chunk2[] = "  13; }\n";
    const char* chunks[] = {chunk1, chunk2, "foo();", NULL};

    RunStreamingTest(chunks);
  }
}


TEST(StreamingUtf8Script) {
  // We'd want to write \uc481 instead of \xec\x92\x81, but Windows compilers
  // don't like it.
  const char* chunk1 =
      "function foo() {\n"
      "  // This function will contain an UTF-8 character which is not in\n"
      "  // ASCII.\n"
      "  var foob\xec\x92\x81r = 13;\n"
      "  return foob\xec\x92\x81r;\n"
      "}\n";
  const char* chunks[] = {chunk1, "foo(); ", NULL};
  RunStreamingTest(chunks, v8::ScriptCompiler::StreamedSource::UTF8);
}


TEST(StreamingUtf8ScriptWithSplitCharactersSanityCheck) {
  // A sanity check to prove that the approach of splitting UTF-8
  // characters is correct. Here is an UTF-8 character which will take three
  // bytes.
  const char* reference = "\xec\x92\x81";
  CHECK(3u == strlen(reference));  // NOLINT - no CHECK_EQ for unsigned.

  char chunk1[] =
      "function foo() {\n"
      "  // This function will contain an UTF-8 character which is not in\n"
      "  // ASCII.\n"
      "  var foob";
  char chunk2[] =
      "XXXr = 13;\n"
      "  return foob\xec\x92\x81r;\n"
      "}\n";
  for (int i = 0; i < 3; ++i) {
    chunk2[i] = reference[i];
  }
  const char* chunks[] = {chunk1, chunk2, "foo();", NULL};
  RunStreamingTest(chunks, v8::ScriptCompiler::StreamedSource::UTF8);
}


TEST(StreamingUtf8ScriptWithSplitCharacters) {
  // Stream data where a multi-byte UTF-8 character is split between two data
  // chunks.
  const char* reference = "\xec\x92\x81";
  char chunk1[] =
      "function foo() {\n"
      "  // This function will contain an UTF-8 character which is not in\n"
      "  // ASCII.\n"
      "  var foobX";
  char chunk2[] =
      "XXr = 13;\n"
      "  return foob\xec\x92\x81r;\n"
      "}\n";
  chunk1[strlen(chunk1) - 1] = reference[0];
  chunk2[0] = reference[1];
  chunk2[1] = reference[2];
  const char* chunks[] = {chunk1, chunk2, "foo();", NULL};
  RunStreamingTest(chunks, v8::ScriptCompiler::StreamedSource::UTF8);
}


TEST(StreamingUtf8ScriptWithSplitCharactersValidEdgeCases) {
  // Tests edge cases which should still be decoded correctly.

  // Case 1: a chunk contains only bytes for a split character (and no other
  // data). This kind of a chunk would be exceptionally small, but we should
  // still decode it correctly.
  const char* reference = "\xec\x92\x81";
  // The small chunk is at the beginning of the split character
  {
    char chunk1[] =
        "function foo() {\n"
        "  // This function will contain an UTF-8 character which is not in\n"
        "  // ASCII.\n"
        "  var foob";
    char chunk2[] = "XX";
    char chunk3[] =
        "Xr = 13;\n"
        "  return foob\xec\x92\x81r;\n"
        "}\n";
    chunk2[0] = reference[0];
    chunk2[1] = reference[1];
    chunk3[0] = reference[2];
    const char* chunks[] = {chunk1, chunk2, chunk3, "foo();", NULL};
    RunStreamingTest(chunks, v8::ScriptCompiler::StreamedSource::UTF8);
  }
  // The small chunk is at the end of a character
  {
    char chunk1[] =
        "function foo() {\n"
        "  // This function will contain an UTF-8 character which is not in\n"
        "  // ASCII.\n"
        "  var foobX";
    char chunk2[] = "XX";
    char chunk3[] =
        "r = 13;\n"
        "  return foob\xec\x92\x81r;\n"
        "}\n";
    chunk1[strlen(chunk1) - 1] = reference[0];
    chunk2[0] = reference[1];
    chunk2[1] = reference[2];
    const char* chunks[] = {chunk1, chunk2, chunk3, "foo();", NULL};
    RunStreamingTest(chunks, v8::ScriptCompiler::StreamedSource::UTF8);
  }
  // Case 2: the script ends with a multi-byte character. Make sure that it's
  // decoded correctly and not just ignored.
  {
    char chunk1[] =
        "var foob\xec\x92\x81 = 13;\n"
        "foob\xec\x92\x81";
    const char* chunks[] = {chunk1, NULL};
    RunStreamingTest(chunks, v8::ScriptCompiler::StreamedSource::UTF8);
  }
}


TEST(StreamingUtf8ScriptWithSplitCharactersInvalidEdgeCases) {
  // Test cases where a UTF-8 character is split over several chunks. Those
  // cases are not supported (the embedder should give the data in big enough
  // chunks), but we shouldn't crash, just produce a parse error.
  const char* reference = "\xec\x92\x81";
  char chunk1[] =
      "function foo() {\n"
      "  // This function will contain an UTF-8 character which is not in\n"
      "  // ASCII.\n"
      "  var foobX";
  char chunk2[] = "X";
  char chunk3[] =
      "Xr = 13;\n"
      "  return foob\xec\x92\x81r;\n"
      "}\n";
  chunk1[strlen(chunk1) - 1] = reference[0];
  chunk2[0] = reference[1];
  chunk3[0] = reference[2];
  const char* chunks[] = {chunk1, chunk2, chunk3, "foo();", NULL};

  RunStreamingTest(chunks, v8::ScriptCompiler::StreamedSource::UTF8, false);
}


TEST(StreamingProducesParserCache) {
  i::FLAG_min_preparse_length = 0;
  const char* chunks[] = {"function foo() { ret", "urn 13; } f", "oo(); ",
                          NULL};

  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);

  v8::ScriptCompiler::StreamedSource source(
      new TestSourceStream(chunks),
      v8::ScriptCompiler::StreamedSource::ONE_BYTE);
  v8::ScriptCompiler::ScriptStreamingTask* task =
      v8::ScriptCompiler::StartStreamingScript(
          isolate, &source, v8::ScriptCompiler::kProduceParserCache);

  // TestSourceStream::GetMoreData won't block, so it's OK to just run the
  // task here in the main thread.
  task->Run();
  delete task;

  const v8::ScriptCompiler::CachedData* cached_data = source.GetCachedData();
  CHECK(cached_data != NULL);
  CHECK(cached_data->data != NULL);
  CHECK(!cached_data->rejected);
  CHECK_GT(cached_data->length, 0);
}


TEST(StreamingWithDebuggingDoesNotProduceParserCache) {
  // If the debugger is active, we should just not produce parser cache at
  // all. This is a regeression test: We used to produce a parser cache without
  // any data in it (just headers).
  i::FLAG_min_preparse_length = 0;
  const char* chunks[] = {"function foo() { ret", "urn 13; } f", "oo(); ",
                          NULL};

  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);

  // Make the debugger active by setting a breakpoint.
  CompileRun("function break_here() { }");
  i::Handle<i::JSFunction> func = i::Handle<i::JSFunction>::cast(
      v8::Utils::OpenHandle(*env->Global()->Get(v8_str("break_here"))));
  v8::internal::Debug* debug = CcTest::i_isolate()->debug();
  int position = 0;
  debug->SetBreakPoint(func, i::Handle<i::Object>(v8::internal::Smi::FromInt(1),
                                                  CcTest::i_isolate()),
                       &position);

  v8::ScriptCompiler::StreamedSource source(
      new TestSourceStream(chunks),
      v8::ScriptCompiler::StreamedSource::ONE_BYTE);
  v8::ScriptCompiler::ScriptStreamingTask* task =
      v8::ScriptCompiler::StartStreamingScript(
          isolate, &source, v8::ScriptCompiler::kProduceParserCache);

  // TestSourceStream::GetMoreData won't block, so it's OK to just run the
  // task here in the main thread.
  task->Run();
  delete task;

  // Check that we got no cached data.
  CHECK(source.GetCachedData() == NULL);
}


TEST(StreamingScriptWithInvalidUtf8) {
  // Regression test for a crash: test that invalid UTF-8 bytes in the end of a
  // chunk don't produce a crash.
  const char* reference = "\xec\x92\x81\x80\x80";
  char chunk1[] =
      "function foo() {\n"
      "  // This function will contain an UTF-8 character which is not in\n"
      "  // ASCII.\n"
      "  var foobXXXXX";  // Too many bytes which look like incomplete chars!
  char chunk2[] =
      "r = 13;\n"
      "  return foob\xec\x92\x81\x80\x80r;\n"
      "}\n";
  for (int i = 0; i < 5; ++i) chunk1[strlen(chunk1) - 5 + i] = reference[i];

  const char* chunks[] = {chunk1, chunk2, "foo();", NULL};
  RunStreamingTest(chunks, v8::ScriptCompiler::StreamedSource::UTF8, false);
}


TEST(StreamingUtf8ScriptWithMultipleMultibyteCharactersSomeSplit) {
  // Regression test: Stream data where there are several multi-byte UTF-8
  // characters in a sequence and one of them is split between two data chunks.
  const char* reference = "\xec\x92\x81";
  char chunk1[] =
      "function foo() {\n"
      "  // This function will contain an UTF-8 character which is not in\n"
      "  // ASCII.\n"
      "  var foob\xec\x92\x81X";
  char chunk2[] =
      "XXr = 13;\n"
      "  return foob\xec\x92\x81\xec\x92\x81r;\n"
      "}\n";
  chunk1[strlen(chunk1) - 1] = reference[0];
  chunk2[0] = reference[1];
  chunk2[1] = reference[2];
  const char* chunks[] = {chunk1, chunk2, "foo();", NULL};
  RunStreamingTest(chunks, v8::ScriptCompiler::StreamedSource::UTF8);
}


TEST(StreamingUtf8ScriptWithMultipleMultibyteCharactersSomeSplit2) {
  // Another regression test, similar to the previous one. The difference is
  // that the split character is not the last one in the sequence.
  const char* reference = "\xec\x92\x81";
  char chunk1[] =
      "function foo() {\n"
      "  // This function will contain an UTF-8 character which is not in\n"
      "  // ASCII.\n"
      "  var foobX";
  char chunk2[] =
      "XX\xec\x92\x81r = 13;\n"
      "  return foob\xec\x92\x81\xec\x92\x81r;\n"
      "}\n";
  chunk1[strlen(chunk1) - 1] = reference[0];
  chunk2[0] = reference[1];
  chunk2[1] = reference[2];
  const char* chunks[] = {chunk1, chunk2, "foo();", NULL};
  RunStreamingTest(chunks, v8::ScriptCompiler::StreamedSource::UTF8);
}


TEST(StreamingWithHarmonyScopes) {
  // Don't use RunStreamingTest here so that both scripts get to use the same
  // LocalContext and HandleScope.
  LocalContext env;
  v8::Isolate* isolate = env->GetIsolate();
  v8::HandleScope scope(isolate);

  // First, run a script with a let variable.
  CompileRun("\"use strict\"; let x = 1;");

  // Then stream a script which (erroneously) tries to introduce the same
  // variable again.
  const char* chunks[] = {"\"use strict\"; let x = 2;", NULL};

  v8::TryCatch try_catch;
  v8::ScriptCompiler::StreamedSource source(
      new TestSourceStream(chunks),
      v8::ScriptCompiler::StreamedSource::ONE_BYTE);
  v8::ScriptCompiler::ScriptStreamingTask* task =
      v8::ScriptCompiler::StartStreamingScript(isolate, &source);
  task->Run();
  delete task;

  // Parsing should succeed (the script will be parsed and compiled in a context
  // independent way, so the error is not detected).
  CHECK_EQ(false, try_catch.HasCaught());

  v8::ScriptOrigin origin(v8_str("http://foo.com"));
  char* full_source = TestSourceStream::FullSourceString(chunks);
  v8::Handle<Script> script = v8::ScriptCompiler::Compile(
      isolate, &source, v8_str(full_source), origin);
  CHECK(!script.IsEmpty());
  CHECK_EQ(false, try_catch.HasCaught());

  // Running the script exposes the error.
  v8::Handle<Value> result(script->Run());
  CHECK(result.IsEmpty());
  CHECK(try_catch.HasCaught());
  delete[] full_source;
}


void TestInvalidCacheData(v8::ScriptCompiler::CompileOptions option) {
  const char* garbage = "garbage garbage garbage garbage garbage garbage";
  const uint8_t* data = reinterpret_cast<const uint8_t*>(garbage);
  int length = 16;
  v8::ScriptCompiler::CachedData* cached_data =
      new v8::ScriptCompiler::CachedData(data, length);
  DCHECK(!cached_data->rejected);
  v8::ScriptOrigin origin(v8_str("origin"));
  v8::ScriptCompiler::Source source(v8_str("42"), origin, cached_data);
  v8::Handle<v8::Script> script =
      v8::ScriptCompiler::Compile(CcTest::isolate(), &source, option);
  CHECK(cached_data->rejected);
  CHECK_EQ(42, script->Run()->Int32Value());
}


TEST(InvalidCacheData) {
  v8::V8::Initialize();
  v8::HandleScope scope(CcTest::isolate());
  LocalContext context;
  TestInvalidCacheData(v8::ScriptCompiler::kConsumeParserCache);
  TestInvalidCacheData(v8::ScriptCompiler::kConsumeCodeCache);
}


TEST(ParserCacheRejectedGracefully) {
  i::FLAG_min_preparse_length = 0;
  v8::V8::Initialize();
  v8::HandleScope scope(CcTest::isolate());
  LocalContext context;
  // Produce valid cached data.
  v8::ScriptOrigin origin(v8_str("origin"));
  v8::Local<v8::String> source_str = v8_str("function foo() {}");
  v8::ScriptCompiler::Source source(source_str, origin);
  v8::Handle<v8::Script> script = v8::ScriptCompiler::Compile(
      CcTest::isolate(), &source, v8::ScriptCompiler::kProduceParserCache);
  CHECK(!script.IsEmpty());
  const v8::ScriptCompiler::CachedData* original_cached_data =
      source.GetCachedData();
  CHECK(original_cached_data != NULL);
  CHECK(original_cached_data->data != NULL);
  CHECK(!original_cached_data->rejected);
  CHECK_GT(original_cached_data->length, 0);
  // Recompiling the same script with it won't reject the data.
  {
    v8::ScriptCompiler::Source source_with_cached_data(
        source_str, origin,
        new v8::ScriptCompiler::CachedData(original_cached_data->data,
                                           original_cached_data->length));
    v8::Handle<v8::Script> script =
        v8::ScriptCompiler::Compile(CcTest::isolate(), &source_with_cached_data,
                                    v8::ScriptCompiler::kConsumeParserCache);
    CHECK(!script.IsEmpty());
    const v8::ScriptCompiler::CachedData* new_cached_data =
        source_with_cached_data.GetCachedData();
    CHECK(new_cached_data != NULL);
    CHECK(!new_cached_data->rejected);
  }
  // Compile an incompatible script with the cached data. The new script doesn't
  // have the same starting position for the function as the old one, so the old
  // cached data will be incompatible with it and will be rejected.
  {
    v8::Local<v8::String> incompatible_source_str =
        v8_str("   function foo() {}");
    v8::ScriptCompiler::Source source_with_cached_data(
        incompatible_source_str, origin,
        new v8::ScriptCompiler::CachedData(original_cached_data->data,
                                           original_cached_data->length));
    v8::Handle<v8::Script> script =
        v8::ScriptCompiler::Compile(CcTest::isolate(), &source_with_cached_data,
                                    v8::ScriptCompiler::kConsumeParserCache);
    CHECK(!script.IsEmpty());
    const v8::ScriptCompiler::CachedData* new_cached_data =
        source_with_cached_data.GetCachedData();
    CHECK(new_cached_data != NULL);
    CHECK(new_cached_data->rejected);
  }
}


TEST(StringConcatOverflow) {
  v8::V8::Initialize();
  v8::HandleScope scope(CcTest::isolate());
  RandomLengthOneByteResource* r =
      new RandomLengthOneByteResource(i::String::kMaxLength);
  v8::Local<v8::String> str = v8::String::NewExternal(CcTest::isolate(), r);
  CHECK(!str.IsEmpty());
  v8::TryCatch try_catch;
  v8::Local<v8::String> result = v8::String::Concat(str, str);
  CHECK(result.IsEmpty());
  CHECK(!try_catch.HasCaught());
}


TEST(TurboAsmDisablesNeuter) {
  v8::V8::Initialize();
  v8::HandleScope scope(CcTest::isolate());
  LocalContext context;
#if V8_TURBOFAN_TARGET
  bool should_be_neuterable = !i::FLAG_turbo_asm;
#else
  bool should_be_neuterable = true;
#endif
  const char* load =
      "function Module(stdlib, foreign, heap) {"
      "  'use asm';"
      "  var MEM32 = new stdlib.Int32Array(heap);"
      "  function load() { return MEM32[0]; }"
      "  return { load: load };"
      "}"
      "var buffer = new ArrayBuffer(4);"
      "Module(this, {}, buffer).load();"
      "buffer";

  i::FLAG_turbo_osr = false;  // TODO(titzer): test requires eager TF.
  v8::Local<v8::ArrayBuffer> result = CompileRun(load).As<v8::ArrayBuffer>();
  CHECK_EQ(should_be_neuterable, result->IsNeuterable());

  const char* store =
      "function Module(stdlib, foreign, heap) {"
      "  'use asm';"
      "  var MEM32 = new stdlib.Int32Array(heap);"
      "  function store() { MEM32[0] = 0; }"
      "  return { store: store };"
      "}"
      "var buffer = new ArrayBuffer(4);"
      "Module(this, {}, buffer).store();"
      "buffer";

  i::FLAG_turbo_osr = false;  // TODO(titzer): test requires eager TF.
  result = CompileRun(store).As<v8::ArrayBuffer>();
  CHECK_EQ(should_be_neuterable, result->IsNeuterable());
}


TEST(GetPrototypeAccessControl) {
  i::FLAG_allow_natives_syntax = true;
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope handle_scope(isolate);
  LocalContext env;

  v8::Handle<v8::ObjectTemplate> obj_template =
      v8::ObjectTemplate::New(isolate);
  obj_template->SetAccessCheckCallbacks(AccessAlwaysBlocked, NULL);

  env->Global()->Set(v8_str("prohibited"), obj_template->NewInstance());

  {
    v8::TryCatch try_catch;
    CompileRun(
        "function f() { %_GetPrototype(prohibited); }"
        "%OptimizeFunctionOnNextCall(f);"
        "f();");
    CHECK(try_catch.HasCaught());
  }
}


TEST(GetPrototypeHidden) {
  i::FLAG_allow_natives_syntax = true;
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope handle_scope(isolate);
  LocalContext env;

  Handle<FunctionTemplate> t = FunctionTemplate::New(isolate);
  t->SetHiddenPrototype(true);
  Handle<Object> proto = t->GetFunction()->NewInstance();
  Handle<Object> object = Object::New(isolate);
  Handle<Object> proto2 = Object::New(isolate);
  object->SetPrototype(proto);
  proto->SetPrototype(proto2);

  env->Global()->Set(v8_str("object"), object);
  env->Global()->Set(v8_str("proto"), proto);
  env->Global()->Set(v8_str("proto2"), proto2);

  v8::Handle<v8::Value> result = CompileRun("%_GetPrototype(object)");
  CHECK(result->Equals(proto2));

  result = CompileRun(
      "function f() { return %_GetPrototype(object); }"
      "%OptimizeFunctionOnNextCall(f);"
      "f()");
  CHECK(result->Equals(proto2));
}


TEST(ClassPrototypeCreationContext) {
  i::FLAG_harmony_classes = true;
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope handle_scope(isolate);
  LocalContext env;

  Handle<Object> result = Handle<Object>::Cast(
      CompileRun("'use strict'; class Example { }; Example.prototype"));
  CHECK(env.local() == result->CreationContext());
}


TEST(SimpleStreamingScriptWithSourceURL) {
  const char* chunks[] = {"function foo() { ret", "urn 13; } f", "oo();\n",
                          "//# sourceURL=bar2.js\n", NULL};
  RunStreamingTest(chunks, v8::ScriptCompiler::StreamedSource::UTF8, true,
                   "bar2.js");
}


TEST(StreamingScriptWithSplitSourceURL) {
  const char* chunks[] = {"function foo() { ret", "urn 13; } f",
                          "oo();\n//# sourceURL=b", "ar2.js\n", NULL};
  RunStreamingTest(chunks, v8::ScriptCompiler::StreamedSource::UTF8, true,
                   "bar2.js");
}


TEST(StreamingScriptWithSourceMappingURLInTheMiddle) {
  const char* chunks[] = {"function foo() { ret", "urn 13; }\n//#",
                          " sourceMappingURL=bar2.js\n", "foo();", NULL};
  RunStreamingTest(chunks, v8::ScriptCompiler::StreamedSource::UTF8, true, NULL,
                   "bar2.js");
}


TEST(NewStringRangeError) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope handle_scope(isolate);
  const int length = i::String::kMaxLength + 1;
  const int buffer_size = length * sizeof(uint16_t);
  void* buffer = malloc(buffer_size);
  if (buffer == NULL) return;
  memset(buffer, 'A', buffer_size);
  {
    v8::TryCatch try_catch;
    char* data = reinterpret_cast<char*>(buffer);
    CHECK(v8::String::NewFromUtf8(isolate, data, v8::String::kNormalString,
                                  length).IsEmpty());
    CHECK(!try_catch.HasCaught());
  }
  {
    v8::TryCatch try_catch;
    uint8_t* data = reinterpret_cast<uint8_t*>(buffer);
    CHECK(v8::String::NewFromOneByte(isolate, data, v8::String::kNormalString,
                                     length).IsEmpty());
    CHECK(!try_catch.HasCaught());
  }
  {
    v8::TryCatch try_catch;
    uint16_t* data = reinterpret_cast<uint16_t*>(buffer);
    CHECK(v8::String::NewFromTwoByte(isolate, data, v8::String::kNormalString,
                                     length).IsEmpty());
    CHECK(!try_catch.HasCaught());
  }
  free(buffer);
}
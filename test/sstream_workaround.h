// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#ifndef BUTIL_TEST_SSTREAM_WORKAROUND
#define BUTIL_TEST_SSTREAM_WORKAROUND

// defining private as public also breaks libstdc++ headers (sstream, any, ...):
// "error: ‘struct std::__cxx11::basic_stringbuf<_CharT, _Traits, _Alloc>::
// __xfer_bufptrs’ redeclared with different access"

#ifdef private
# define BUTIL_TEST_RESTORE_PRIVATE
# undef private
#endif

#ifdef protected
# define BUTIL_TEST_RESTORE_PROTECTED
# undef protected
#endif

#include <sstream>
#include <any>

#ifdef BUTIL_TEST_RESTORE_PRIVATE
# define private public
# undef BUTIL_TEST_RESTORE_PRIVATE
#endif

#ifdef BUTIL_TEST_RESTORE_PROTECTED
# define protected public
# undef BUTIL_TEST_RESTORE_PROTECTED
#endif

#endif  //  BUTIL_TEST_SSTREAM_WORKAROUND

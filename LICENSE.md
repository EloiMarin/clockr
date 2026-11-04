# License

clockr is MIT-licensed. The MIT text is at the end of this file; the
third-party notices below cover the components that are redistributed with
the repository or linked into the library.

## Third-party components

### TLSF

TLSF (Two-Level Segregated Fit memory allocator), version 3.1.

Source: https://github.com/mattconte/tlsf

Copyright (c) 2006-2016, Matthew Conte. All rights reserved.

Licensed under the BSD 3-Clause License. The original copyright notice,
license conditions and disclaimer are available in the TLSF source header.

### ReaderWriterQueue

A single-producer/single-consumer lock-free queue, version 1.0.7.

Source: https://github.com/cameron314/readerwriterqueue

Copyright (c) 2013-2025, Cameron Desrochers. All rights reserved.

Licensed under the BSD 2-Clause License; the full text ships with the headers
in `include/clockr/third_party/readerwriterqueue/LICENSE.md`.

### Eclipse Layout Kernel (elkjs)

The debugger web UI vendors `tools/web-debugger/web/vendor/elk.bundled.js`.

Copyright (c) the ELK/elkjs contributors.

Licensed under the Eclipse Public License 2.0; the full text is in
`tools/web-debugger/web/vendor/ELK-LICENSE`, and provenance is recorded in
`tools/web-debugger/web/vendor/README.md`. The bundle is vendored unmodified
and used as a separate library; the MIT terms below cover the rest of this
project.

## Build and test dependencies

Catch2, protobuf, abseil, uWebSockets, uSockets, and libuv on Windows are
fetched by CMake and are not redistributed with the library. Each keeps its
own upstream license.

## MIT License

Copyright (c) 2026 Eloi Marín Gratacós

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

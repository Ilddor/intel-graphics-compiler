/*===================== begin_copyright_notice ==================================

Copyright (c) 2017 Intel Corporation

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be included
in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.


======================= end_copyright_notice ==================================*/

#include "strings.hpp"
#include <string.h>
#include <sstream>

using namespace iga;

#ifdef _WIN32
#include <Windows.h>
#include <malloc.h> /* for alloca */
#else
#include <alloca.h> /* for alloca */
#endif

std::string iga::format(const char *pat, ...)
{
    va_list va;
    va_start(va, pat);
    std::string s = formatv(pat, va);
    va_end(va);
    return s;
}


std::string iga::formatv(const char *pat, va_list &va)
{
    va_list copy;
    va_copy(copy, va);
    size_t ebuflen = VSCPRINTF(pat, copy) + 1;
    va_end(copy);

    char *buf = (char *)alloca(ebuflen);
    VSPRINTF(buf, ebuflen, pat, va);
    buf[ebuflen - 1] = 0;

    return buf;
}


size_t iga::formatTo(std::ostream &out, const char *pat, ...)
{
    va_list va;
    va_start(va, pat);
    size_t s = formatvTo(out, pat, va);
    va_end(va);
    return s;
}


size_t iga::formatvTo(std::ostream &out, const char *pat, va_list &va)
{
    va_list copy;
    va_copy(copy, va);
    size_t ebuflen = VSCPRINTF(pat, copy) + 1;
    va_end(copy);

    char *buf = (char *)alloca(ebuflen);
    VSPRINTF(buf, ebuflen, pat, va);
    buf[ebuflen - 1] = 0;

    size_t initOff = (size_t)out.tellp();
    out << buf;
    return (size_t)out.tellp() - initOff;
}


size_t iga::formatTo(char *buf, size_t bufLen, const char *pat, ...)
{
    std::stringstream ss;
    va_list va;
    va_start(va, pat);
    formatvTo(ss, pat, va);
    va_end(va);
    return copyOut(buf, bufLen, ss);
}


size_t iga::formatvTo(char *buf, size_t bufLen, const char *pat, va_list &va)
{
    return VSPRINTF(buf, bufLen, pat, va);
}


size_t iga::copyOut(char *buf, size_t bufCap, std::iostream &ios)
{
    size_t sslen = (size_t)ios.tellp();
    if (!buf || bufCap == 0)
        return sslen;
    ios.read(buf, bufCap);
    size_t eot = bufCap - 1 < sslen ? bufCap - 1 : sslen;
    buf[eot] = '\0';
    return sslen;
}

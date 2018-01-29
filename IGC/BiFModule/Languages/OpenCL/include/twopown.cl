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

#ifndef __TWOPOWN_CL__
#define __TWOPOWN_CL__

/******************************************************************
Function: twopown

Description: This function calculates 2^n for integer values of n.

******************************************************************/
float twopown( int n )
{
    float result;
    int man, exp;

    if( n > 126 )
    {
        exp = 255;
        man = 0;
    }
    else if( n > -127 )
    {
        exp = n + 127;
        man = 0;
    }
    else if( n > -149 )
    {
        exp = 0;
        man = 0x00400000 >> abs( n + 127 );
    }
    else
    {
        exp = 0;
        man = 0;
    }

    return as_float( (exp << FLOAT_MANTISSA_BITS ) | man );
}


#endif

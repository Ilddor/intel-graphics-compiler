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

#include <cstring>
#include "common/ged_ins_restrictions.h"

using std::memcmp;
using std::memset;


const char* gedRestrictionTypeStrings[GED_FIELD_RESTRICTIONS_TYPE_SIZE] =
{
    "GED_FIELD_RESTRICTIONS_TYPE_NONE",
    "GED_FIELD_RESTRICTIONS_TYPE_VALUE",
    "GED_FIELD_RESTRICTIONS_TYPE_RANGE",
    "GED_FIELD_RESTRICTIONS_TYPE_MASK",
    "GED_FIELD_RESTRICTIONS_TYPE_PADDING",
    "GED_FIELD_RESTRICTIONS_TYPE_FIELD_TYPE",
    "GED_FIELD_RESTRICTIONS_TYPE_ENUM"
};


const char* gedRestrictionTypePadding[GED_FIELD_RESTRICTIONS_TYPE_SIZE] =
{
    "      ",
    "     ",
    "     ",
    "      ",
    "   ",
    "",
    "      "
};


const char* ged_field_restriction_t_str = "ged_field_restriction_t";


const char* gedRestrictionTypeNames[GED_FIELD_RESTRICTIONS_TYPE_SIZE] =
{
    "",
    "Value",
    "Range",
    "Mask",
    "Padding",
    "Field Type",
    "Enum"
};


bool ged_field_restriction_t::operator==(const ged_field_restriction_t& rhs) const
{
    if (_restrictionType != rhs._restrictionType) return false;
    return (0 == memcmp(this->_dummy._cvsa, rhs._dummy._cvsa, sizeof(field_restriction_union_initializer_t)));
}


bool ged_field_restriction_t::operator<(const ged_field_restriction_t& rhs) const
{
    if (_restrictionType != rhs._restrictionType) return (_restrictionType < rhs._restrictionType);
    return (memcmp(this->_dummy._cvsa, rhs._dummy._cvsa, sizeof(field_restriction_union_initializer_t)) < 0);
}


void InitRestriction(ged_field_restriction_t& restriction)
{
    restriction._restrictionType = GED_FIELD_RESTRICTIONS_TYPE_NONE;
    memset(&(restriction._dummy), 0, sizeof(restriction._dummy));
}

// ckplot — professional charts for modern C++
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
#include "cplot/units.hpp"

#include <cstdio>
#include <cstdlib>

#include <cworks/app_error.hpp>

#include "cplot/error.hpp"
#include "format_c.hpp"

namespace cplot {

std::string Length::svg_attribute() const {
    const char* suffix = "";
    switch (unit) {
    case Unit::Px: suffix = ""; break;
    case Unit::Mm: suffix = "mm"; break;
    case Unit::Cm: suffix = "cm"; break;
    case Unit::In: suffix = "in"; break;
    case Unit::Pt: suffix = "pt"; break;
    }
    // C locale so the dimension radix is '.' regardless of LC_NUMERIC.
    return detail::format_c("%g%s", value, suffix);
}

Length parse_length(const std::string& text) {
    char* end = nullptr;
    // C-locale strtod: "12.5cm" must parse the same in every process locale;
    // plain strtod under de_DE would stop at the '.' (see format_c.hpp).
    const double v = detail::strtod_c(text.c_str(), &end);
    if (end == text.c_str())
        throw Error(cworks::validation_failed("invalid length: '" + text + "'"));
    const std::string suffix = end;
    Length::Unit unit;
    if (suffix.empty() || suffix == "px") unit = Length::Unit::Px;
    else if (suffix == "mm") unit = Length::Unit::Mm;
    else if (suffix == "cm") unit = Length::Unit::Cm;
    else if (suffix == "in") unit = Length::Unit::In;
    else if (suffix == "pt") unit = Length::Unit::Pt;
    else
        throw Error(cworks::validation_failed("invalid length unit '" + suffix + "' in '" + text +
                                              "' (px, mm, cm, in, pt)"));
    if (v <= 0)
        throw Error(cworks::validation_failed("length must be positive: '" + text + "'"));
    return {v, unit};
}

} // namespace cplot

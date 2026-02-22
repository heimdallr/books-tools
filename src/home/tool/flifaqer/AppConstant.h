#pragma once

namespace HomeCompa::FliFaq::Constant
{

inline constexpr auto TEMPLATE    = "template";
inline constexpr auto INPUT_FILES = "files";
inline constexpr auto THEME       = "theme";

constexpr auto ERROR = QT_TRANSLATE_NOOP("flifaqer", "Error");

}

namespace HomeCompa::FliFaq
{

inline QString Tr(const char* str)
{
	return QCoreApplication::translate("flifaqer", str);
}

}

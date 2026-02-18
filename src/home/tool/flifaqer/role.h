#pragma once

#include "qnamespace.h"

namespace HomeCompa::FliFaq
{

struct Role
{
	enum
	{
		AddFile = Qt::UserRole,
		LanguageList,
		Language,
		ReferenceLanguage,
		ReferenceQuestion,
		ReferenceAnswer,
		TranslationLanguage,
		TranslationQuestion,
		TranslationAnswer,
		Macro,
		Save,
		Export,
		Validate,
	};
};

}

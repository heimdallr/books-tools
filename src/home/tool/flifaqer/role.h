#pragma once

#include "qnamespace.h"

namespace HomeCompa::FliFaq
{

struct Role
{
	enum
	{
		LanguageList = Qt::UserRole,
		Language,
		ReferenceLanguage,
		ReferenceQuestion,
		ReferenceAnswer,
		TranslationLanguage,
		TranslationQuestion,
		TranslationAnswer,
		Title,
		Head,
		Tail,
		Save,
		Export,
	};
};

}

#pragma once

#include "qnamespace.h"

namespace HomeCompa::FliFaq
{

struct Role
{
	enum
	{
		AddFile = Qt::UserRole,
		AddTemplate,
		QuestionTypeList,
		LanguageList,
		Language,
		ReferenceLanguage,
		ReferenceQuestion,
		ReferenceAnswer,
		ReferenceText,
		TranslationLanguage,
		TranslationQuestion,
		TranslationAnswer,
		TranslationText,
		TemplateQuestion,
		TemplateAnswer,
		Macro,
		Save,
		Export,
		Validate,
	};
};

}

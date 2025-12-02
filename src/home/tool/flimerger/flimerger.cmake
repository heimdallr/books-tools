AddTarget(flimerger	app_console
	PROJECT_GROUP Tool
	SOURCE_DIRECTORY
		"${CMAKE_CURRENT_LIST_DIR}"
	LINK_LIBRARIES
		Boost::headers
		Qt${QT_MAJOR_VERSION}::Core
	LINK_TARGETS
		lib
		logging
		util
		zip
)

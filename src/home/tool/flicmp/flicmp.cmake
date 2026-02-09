AddTarget(flicmp	 app_console
	PROJECT_GROUP    Tool
	SOURCE_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}"
	SKIP_INSTALL
	LINK_LIBRARIES
		Boost::headers
		Qt${QT_MAJOR_VERSION}::Core
		Qt${QT_MAJOR_VERSION}::Gui
	LINK_TARGETS
		lib
		logging
		util
		zip
)

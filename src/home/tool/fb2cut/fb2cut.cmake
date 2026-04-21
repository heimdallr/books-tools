AddTarget(fb2cut	app_console
	PROJECT_GROUP Tool
	SOURCE_DIRECTORY
		"${CMAKE_CURRENT_LIST_DIR}"
	LINK_LIBRARIES
		Boost::headers
		Qt${QT_MAJOR_VERSION}::Core
		Qt${QT_MAJOR_VERSION}::Gui
		Qt${QT_MAJOR_VERSION}::Svg
	LINK_TARGETS
		flicu
		fljxl
		lib
		logging
		util
		zip
)

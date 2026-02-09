AddTarget(lib	shared_lib
	PROJECT_GROUP Tool/Util
	SOURCE_DIRECTORY
		"${CMAKE_CURRENT_LIST_DIR}"
	LINK_LIBRARIES
		Qt${QT_MAJOR_VERSION}::Core
		Qt${QT_MAJOR_VERSION}::Gui
		cimg::cimg
	LINK_TARGETS
		dbfactory
		logging
		util
		zip
)

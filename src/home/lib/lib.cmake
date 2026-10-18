AddTarget(lib	shared_lib
	PROJECT_GROUP Tool/Util
	SOURCE_DIRECTORY
		"${CMAKE_CURRENT_LIST_DIR}"
	LINK_LIBRARIES
		Boost::headers
		Qt${QT_MAJOR_VERSION}::Core
	LINK_TARGETS
		dbfactory
		logging
		platform
		util
		zip
)

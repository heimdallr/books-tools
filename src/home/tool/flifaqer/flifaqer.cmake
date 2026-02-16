CreateWinRC(app
    	COMPANY_NAME      "HomeCompa"
    	FILE_NAME         "flifaqer"
    	FILE_DESCRIPTION  "FAQ translation util"
    	APP_ICON          "${CMAKE_CURRENT_LIST_DIR}/resources/icons/main.ico"
    	APP_VERSION       ${PRODUCT_VERSION}
)

AddTarget(flifaqer	app
	PROJECT_GROUP App
	WIN_RC ${CMAKE_BINARY_DIR}/resources/app.rc
	SOURCE_DIRECTORY
		"${CMAKE_CURRENT_LIST_DIR}"
	LINK_LIBRARIES
		Boost::headers
		Qt${QT_MAJOR_VERSION}::Widgets
	LINK_TARGETS
		logging
		util
	QT_PLUGINS
		qwindows
		qmodernwindowsstyle
)

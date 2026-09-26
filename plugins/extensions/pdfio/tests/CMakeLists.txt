# The backend runs the same code the plugin runs; the test compiles it directly so it can
# be exercised with ctest instead of having to start the whole application.
if (Poppler_Qt${QT_MAJOR_VERSION}_FOUND)
    add_executable(PdfRenderBackendTest
        PdfRenderBackendTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../backends/poppler/PopplerRenderBackend.cpp
        )

    target_include_directories(PdfRenderBackendTest PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/..)

    target_link_libraries(PdfRenderBackendTest PRIVATE
        Qt${QT_MAJOR_VERSION}::Core
        Qt${QT_MAJOR_VERSION}::Gui
        Qt${QT_MAJOR_VERSION}::Test
        Poppler::Qt${QT_MAJOR_VERSION})

    target_compile_definitions(PdfRenderBackendTest PRIVATE
        PDFIO_HAVE_POPPLER
        FILES_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data/")

    set_target_properties(PdfRenderBackendTest PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

    add_test(NAME pdfio-PdfRenderBackendTest COMMAND PdfRenderBackendTest)
    set_tests_properties(pdfio-PdfRenderBackendTest PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    add_executable(PdfSessionTest
        PdfSessionTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../backends/poppler/PopplerRenderBackend.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../session/PdfNotebookOps.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../session/PdfNotebookQuicks.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../session/PdfSession.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../session/PdfSessionManifest.cpp
        )

    target_include_directories(PdfSessionTest PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/..)

    target_link_libraries(PdfSessionTest PRIVATE
        Qt${QT_MAJOR_VERSION}::Core
        Qt${QT_MAJOR_VERSION}::Gui
        Qt${QT_MAJOR_VERSION}::Test
        Poppler::Qt${QT_MAJOR_VERSION})

    target_compile_definitions(PdfSessionTest PRIVATE
        PDFIO_HAVE_POPPLER
        FILES_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data/")

    set_target_properties(PdfSessionTest PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

    add_test(NAME pdfio-PdfSessionTest COMMAND PdfSessionTest)
    set_tests_properties(pdfio-PdfSessionTest PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    add_executable(PdfProjectBuilderTest
        PdfProjectBuilderTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../backends/poppler/PopplerRenderBackend.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../session/PdfSession.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../session/PdfSessionManifest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../session/PdfProjectBuilder.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../session/PdfSourceRenderers.cpp
        )

    target_include_directories(PdfProjectBuilderTest PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/..)

    target_link_libraries(PdfProjectBuilderTest PRIVATE
        Qt${QT_MAJOR_VERSION}::Core
        Qt${QT_MAJOR_VERSION}::Gui
        Qt${QT_MAJOR_VERSION}::Test
        Poppler::Qt${QT_MAJOR_VERSION}
        kritaimage)

    target_compile_definitions(PdfProjectBuilderTest PRIVATE
        PDFIO_HAVE_POPPLER
        FILES_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data/")

    set_target_properties(PdfProjectBuilderTest PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

    add_test(NAME pdfio-PdfProjectBuilderTest COMMAND PdfProjectBuilderTest)
    set_tests_properties(pdfio-PdfProjectBuilderTest PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    add_executable(PdfExporterTest
        PdfExporterTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../backends/poppler/PopplerRenderBackend.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../session/PdfExporter.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../session/PdfNotebookOps.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../session/PdfSession.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../session/PdfSessionManifest.cpp
        )

    target_include_directories(PdfExporterTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)

    target_link_libraries(PdfExporterTest PRIVATE
        Qt${QT_MAJOR_VERSION}::Core
        Qt${QT_MAJOR_VERSION}::Gui
        Qt${QT_MAJOR_VERSION}::Test
        Poppler::Qt${QT_MAJOR_VERSION})

    target_compile_definitions(PdfExporterTest PRIVATE
        PDFIO_HAVE_POPPLER
        FILES_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data/")

    set_target_properties(PdfExporterTest PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

    # The bounded page window is plain bookkeeping over the manifest, so it compiles into a test
    # without Poppler, Krita or a QPA platform -- which is the point: the rule that a dirty page is
    # never evicted can be exercised in ctest instead of only inside a running application.
    add_executable(PdfPageWindowTest
        PdfPageWindowTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../session/PdfPageWindow.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../session/PdfSession.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../session/PdfSessionManifest.cpp
        )

    target_include_directories(PdfPageWindowTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)

    target_link_libraries(PdfPageWindowTest PRIVATE
        Qt${QT_MAJOR_VERSION}::Core
        Qt${QT_MAJOR_VERSION}::Gui
        Qt${QT_MAJOR_VERSION}::Test)

    set_target_properties(PdfPageWindowTest PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

    add_test(NAME pdfio-PdfPageWindowTest COMMAND PdfPageWindowTest)
    set_tests_properties(pdfio-PdfPageWindowTest PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # The save queue is the pipeline every page write goes through -- one at a time, in order,
    # with a watchdog for the write that never reports -- and like PdfPageWindow it is plain
    # bookkeeping over std::functions, so the rule that no two writes are ever in the air at once
    # can be exercised in ctest instead of only by scrolling a notebook on the tablet.
    add_executable(PdfSaveQueueTest
        PdfSaveQueueTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../session/PdfSaveQueue.cpp
        )

    target_include_directories(PdfSaveQueueTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)

    target_link_libraries(PdfSaveQueueTest PRIVATE
        Qt${QT_MAJOR_VERSION}::Core
        Qt${QT_MAJOR_VERSION}::Gui
        Qt${QT_MAJOR_VERSION}::Test)

    set_target_properties(PdfSaveQueueTest PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

    add_test(NAME pdfio-PdfSaveQueueTest COMMAND PdfSaveQueueTest)
    set_tests_properties(pdfio-PdfSaveQueueTest PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    add_executable(PdfStripLayoutTest
        PdfStripLayoutTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../session/PdfSessionManifest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../session/PdfStripLayout.cpp
        )

    target_include_directories(PdfStripLayoutTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)

    target_link_libraries(PdfStripLayoutTest PRIVATE
        Qt${QT_MAJOR_VERSION}::Core
        Qt${QT_MAJOR_VERSION}::Gui
        Qt${QT_MAJOR_VERSION}::Test)

    set_target_properties(PdfStripLayoutTest PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

    add_test(NAME pdfio-PdfStripLayoutTest COMMAND PdfStripLayoutTest)
    set_tests_properties(pdfio-PdfStripLayoutTest PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    add_executable(PdfStripBuilderTest
        PdfStripBuilderTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../backends/poppler/PopplerRenderBackend.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../session/PdfInkLoader.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../session/PdfSessionManifest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../session/PdfSourceRenderers.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../session/PdfStripBuilder.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../session/PdfStripLayout.cpp
        )

    target_include_directories(PdfStripBuilderTest PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/..
        ${CMAKE_SOURCE_DIR}/plugins/impex/libkra)

    target_link_libraries(PdfStripBuilderTest PRIVATE
        Qt${QT_MAJOR_VERSION}::Core
        Qt${QT_MAJOR_VERSION}::Gui
        Qt${QT_MAJOR_VERSION}::Test
        Poppler::Qt${QT_MAJOR_VERSION}
        kritaimage
        kritalibkra
        KF${KF_MAJOR}::Archive)

    target_compile_definitions(PdfStripBuilderTest PRIVATE
        PDFIO_HAVE_POPPLER
        FILES_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data/")

    set_target_properties(PdfStripBuilderTest PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

    add_test(NAME pdfio-PdfStripBuilderTest COMMAND PdfStripBuilderTest)
    set_tests_properties(pdfio-PdfStripBuilderTest PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    add_test(NAME pdfio-PdfExporterTest COMMAND PdfExporterTest)
    set_tests_properties(pdfio-PdfExporterTest PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # The one test that turns a real page. PdfPageWindowTest proves the eviction rule by itself
    # and PdfProjectBuilderTest proves the layer contract, but neither ever drives the navigator:
    # the save that has to run before an eviction is PdfPageNavigator::saveCurrentPage, and the
    # document it saves into is a real KisDocument built through KisPart. Both are linked in
    # here, which is what the earlier "PdfPageSaver is deliberately not covered" note said was
    # missing: a QTest with no KisPart, no main window and no plugin path hung rather than failed.
    #
    # The plugins are found through KRITA_PLUGIN_PATH. The save is Krita's .kra export, which is
    # itself a plugin, so the test points that variable at the built plugins and depends on the
    # export plugin being built rather than at an installed copy of Krita.
    # The main window is set up from resources that live in the application's own .qrc, not in
    # kritaui: without :/kxmlgui5/krita5.xmlgui a KisMainWindow has no toolbar action and adding a
    # view walks into KDE's ToolBarHandler with an empty action list and crashes. Krita's own ui
    # tests add the same file (libs/ui/tests/CMakeLists.txt, kis_view_signals_test).
    add_executable(PdfNavigatorIntegrationTest
        PdfNavigatorIntegrationTest.cpp
        "${CMAKE_BINARY_DIR}/krita/krita.qrc"
        ${CMAKE_CURRENT_SOURCE_DIR}/../backend/PdfRenderBackendCreate.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../backends/poppler/PopplerRenderBackend.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../PdfNotebookOpsDialog.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../PdfPageNavigator.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../PdfPageStripDecoration.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../session/PdfInkLoader.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../session/PdfPageRotator.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../session/PdfPageSaver.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../session/PdfPageWindow.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../session/PdfProjectBuilder.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../session/PdfSaveQueue.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../session/PdfSession.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../session/PdfSessionManifest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../session/PdfNotebookOps.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../session/PdfSourceRenderers.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../session/PdfStripBuilder.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../session/PdfStripLayout.cpp
        )

    # Two plugins the page turn cannot run without, and neither is in the test binary: the ink-only
    # save is Krita's .kra export (Krita/FileFilter) and the ICC profiles the canvas asks for come
    # from the color space engine (Krita/ColorSpace). Both are found through KRITA_PLUGIN_PATH,
    # which points at the directory they are built into.
    add_dependencies(PdfNavigatorIntegrationTest
        kritakraexport
        kritalcmsengine)

    target_include_directories(PdfNavigatorIntegrationTest PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/..
        ${CMAKE_SOURCE_DIR}/plugins/impex/libkra)

    target_link_libraries(PdfNavigatorIntegrationTest PRIVATE
        Qt${QT_MAJOR_VERSION}::Core
        Qt${QT_MAJOR_VERSION}::Gui
        Qt${QT_MAJOR_VERSION}::Widgets
        Qt${QT_MAJOR_VERSION}::Test
        Poppler::Qt${QT_MAJOR_VERSION}
        kritaui
        kritaimage
        kritalibkra
        kritawidgetutils
        KF${KF_MAJOR}::Archive)

    # KRITA_RESOURCE_DIRS_FOR_TESTS is not set here: it holds a semicolon-separated list, which a
    # compile definition cannot carry (CMake splits it and moc is handed an argument that is not a
    # macro). It comes from the generated <KoTestConfig.h> instead, the same header Krita's own
    # tests read it from (sdk/tests/kistest.h).
    target_compile_definitions(PdfNavigatorIntegrationTest PRIVATE
        PDFIO_HAVE_POPPLER
        FILES_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data/"
        PDFIO_PLUGIN_DIR="$<TARGET_FILE_DIR:kritakraexport>")

    set_target_properties(PdfNavigatorIntegrationTest PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
        AUTORCC ON)

    add_test(NAME pdfio-PdfNavigatorIntegrationTest COMMAND PdfNavigatorIntegrationTest)
    set_tests_properties(pdfio-PdfNavigatorIntegrationTest PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QTEST_FUNCTION_TIMEOUT=120000"
        TIMEOUT 600)

    # The bundle is a zip of a project directory: one file in, one file out. Reading one is the
    # dangerous half -- zip slip, an unknown schema, a source that does not hash to what the
    # manifest recorded, a manifest naming a page the file does not carry, a truncated or edited
    # archive -- and all of it is pure logic over an archive, so it belongs in ctest rather than
    # only inside a running Krita. The archive is the same KArchive the plugin already reads a .kra
    # with, which is why the whole feature costs no new dependency.

    add_executable(PdfNotebookBundleTest
        PdfNotebookBundleTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../backends/poppler/PopplerRenderBackend.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../session/PdfNotebookBundle.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../session/PdfSession.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../session/PdfSessionManifest.cpp
        )

    target_include_directories(PdfNotebookBundleTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)

    target_link_libraries(PdfNotebookBundleTest PRIVATE
        Qt${QT_MAJOR_VERSION}::Core
        Qt${QT_MAJOR_VERSION}::Gui
        Qt${QT_MAJOR_VERSION}::Test
        Poppler::Qt${QT_MAJOR_VERSION}
        KF${KF_MAJOR}::Archive)

    target_compile_definitions(PdfNotebookBundleTest PRIVATE
        PDFIO_HAVE_POPPLER
        FILES_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data/")

    set_target_properties(PdfNotebookBundleTest PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

    add_test(NAME pdfio-PdfNotebookBundleTest COMMAND PdfNotebookBundleTest)
    set_tests_properties(pdfio-PdfNotebookBundleTest PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # The scroll-follow bug, as a state machine over real strip geometry. checkScrollFollow() is a
    # private member of a KisPart singleton, so the test drives a line-referenced replica of it
    # against PdfStripLayout, which is where the fix's primitive lives -- see
    # PdfStripLayout::nearestPage and docs/verify/MULTIPAGE-RENDER-TEST.md.
    add_executable(PdfStripScrollFollowTest
        PdfStripScrollFollowTest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../session/PdfStripLayout.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../session/PdfSessionManifest.cpp
        )

    target_include_directories(PdfStripScrollFollowTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)

    target_link_libraries(PdfStripScrollFollowTest PRIVATE
        Qt${QT_MAJOR_VERSION}::Core
        Qt${QT_MAJOR_VERSION}::Gui
        Qt${QT_MAJOR_VERSION}::Test)

    set_target_properties(PdfStripScrollFollowTest PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

    add_test(NAME pdfio-PdfStripScrollFollowTest COMMAND PdfStripScrollFollowTest)
    set_tests_properties(pdfio-PdfStripScrollFollowTest PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # The three-slot strip over real documents: rendering into every slot, cursor to page, node
    # activation on a real view, locking as the guarantee, and what a page folder with several
    # layers does across a save and a reload. It needs KisPart, a main window and a view for the
    # activation half, so it copies the harness PdfNavigatorIntegrationTest established.
    add_executable(PdfStripCursorTest
        PdfStripCursorTest.cpp
        "${CMAKE_BINARY_DIR}/krita/krita.qrc"
        ${CMAKE_CURRENT_SOURCE_DIR}/../backend/PdfRenderBackendCreate.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../backends/poppler/PopplerRenderBackend.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../session/PdfInkLoader.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../session/PdfPageSaver.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../session/PdfProjectBuilder.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../session/PdfSessionManifest.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../session/PdfSourceRenderers.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../session/PdfStripBuilder.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../session/PdfStripLayout.cpp
        )

    add_dependencies(PdfStripCursorTest
        kritakraexport
        kritalcmsengine)

    target_include_directories(PdfStripCursorTest PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/..
        ${CMAKE_SOURCE_DIR}/plugins/impex/libkra)

    target_link_libraries(PdfStripCursorTest PRIVATE
        Qt${QT_MAJOR_VERSION}::Core
        Qt${QT_MAJOR_VERSION}::Gui
        Qt${QT_MAJOR_VERSION}::Widgets
        Qt${QT_MAJOR_VERSION}::Test
        Poppler::Qt${QT_MAJOR_VERSION}
        kritaui
        kritaimage
        kritawidgetutils
        kritalibkra
        KF${KF_MAJOR}::Archive)

    target_compile_definitions(PdfStripCursorTest PRIVATE
        PDFIO_HAVE_POPPLER
        FILES_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data/"
        PDFIO_PLUGIN_DIR="$<TARGET_FILE_DIR:kritakraexport>")

    set_target_properties(PdfStripCursorTest PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
        AUTORCC ON)

    add_test(NAME pdfio-PdfStripCursorTest COMMAND PdfStripCursorTest)
    set_tests_properties(pdfio-PdfStripCursorTest PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QTEST_FUNCTION_TIMEOUT=120000"
        TIMEOUT 600)

endif()

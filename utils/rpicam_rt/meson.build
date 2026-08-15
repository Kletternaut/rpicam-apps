# meson.build for rpicam-rt (optional Qt runtime control panel)

enable_rpicam_rt = get_option('enable_rpicam_rt')
rpicam_rt_enabled = false

if not enable_rpicam_rt.disabled()
    qt_dep = dependency('qt6', modules : ['Core', 'Widgets', 'Network'], required : false)

    if qt_dep.found()
        qt = import('qt6')
    else
        message('Qt6 libraries not found, falling back to Qt5')
        qt_dep = dependency('qt5',
            modules : ['Core', 'Widgets', 'Network'],
            required : enable_rpicam_rt)
        if qt_dep.found()
            qt = import('qt5')
        endif
    endif

    if qt_dep.found()
        rpicam_rt_enabled = true
        moc_files = qt.preprocess(
            moc_sources : ['main.cpp'],
            qresources : ['resources.qrc'],
            dependencies : qt_dep,
            include_directories : include_directories('.'),
        )

        executable('rpicam-rt',
            sources : ['main.cpp', moc_files],
            dependencies : qt_dep,
            include_directories : include_directories('../..'),
            install : true,
        )

        desktop_conf = configuration_data()
        desktop_conf.set('BINDIR', get_option('prefix') / get_option('bindir'))

        desktop_file = configure_file(
            input : 'rpicam-rt.desktop.in',
            output : 'rpicam-rt.desktop',
            configuration : desktop_conf,
        )

        install_data(desktop_file,
            install_dir : get_option('datadir') / 'applications',
        )

        install_data('rpicam-rt.svg',
            install_dir : get_option('datadir') / 'icons' / 'hicolor' / 'scalable' / 'apps',
        )
    endif
endif

libseat = dependency('libseat',
	version: '>=0.2.0',
	fallback: 'seatd',
	default_options: ['server=disabled', 'man-pages=disabled'],
)
wlr_files += files('session.c')
wlr_deps += libseat

# libudev & libdemi

if get_option('enum-backend') == 'auto'
	udev = dependency('libudev', required: false)
	demi = dependency('demi', required: false)
	if udev.found()
		wlr_files += files('udev.c')
		wlr_deps += dependency('libudev')
		features += { 'udev': true }
	elif demi.found()
		wlr_files += files('demi.c')
		wlr_deps += dependency('demi')
		features += { 'demi': true }
	endif
elif get_option('enum-backend') == 'udev'
	wlr_files += files('udev.c')
	wlr_deps += dependency('libudev')
	features += { 'udev': true }
elif get_option('enum-backend') == 'demi'
	wlr_files += files('demi.c')
	wlr_deps += dependency('demi')
	features += { 'demi': true }
endif

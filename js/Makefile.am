NULL =

EXTRA_DIST = misc/config.js.in
CLEANFILES = misc/config.js

misc/config.js: misc/config.js.in Makefile
	[ -d $(@D) ] || $(mkdir_p) $(@D) ; \
	sed -e "s|[@]PACKAGE_NAME@|$(PACKAGE_NAME)|g" \
	    -e "s|[@]PACKAGE_VERSION@|$(PACKAGE_VERSION)|g" \
	    -e "s|[@]HAVE_BLUETOOTH@|$(HAVE_BLUETOOTH)|g" \
	    -e "s|[@]GETTEXT_PACKAGE@|$(GETTEXT_PACKAGE)|g" \
	    -e "s|[@]datadir@|$(datadir)|g" \
	    -e "s|[@]libexecdir@|$(libexecdir)|g" \
	    -e "s|[@]sysconfdir@|$(sysconfdir)|g" \
               $< > $@

jsdir = $(pkgdatadir)/js

nobase_dist_js_DATA = 	\
	gdm/batch.js		\
	gdm/fingerprint.js	\
	gdm/loginDialog.js	\
	gdm/powerMenu.js	\
	gdm/realmd.js		\
	gdm/util.js		\
	extensionPrefs/main.js	\
	misc/config.js		\
	misc/extensionUtils.js	\
	misc/fileUtils.js	\
	misc/gnomeSession.js	\
	misc/history.js		\
	misc/jsParse.js		\
	misc/loginManager.js	\
	misc/modemManager.js	\
	misc/params.js		\
	misc/util.js		\
	perf/core.js		\
	ui/altTab.js		\
	ui/appDisplay.js	\
	ui/appFavorites.js	\
	ui/boxpointer.js	\
	ui/calendar.js		\
	ui/checkBox.js		\
	ui/ctrlAltTab.js	\
	ui/dash.js		\
	ui/dateMenu.js		\
	ui/dnd.js		\
	ui/endSessionDialog.js	\
	ui/extensionSystem.js	\
	ui/extensionDownloader.js \
	ui/environment.js	\
	ui/flashspot.js		\
	ui/ibusCandidatePopup.js\
	ui/grabHelper.js	\
	ui/iconGrid.js		\
	ui/keyboard.js		\
	ui/layout.js		\
	ui/lightbox.js		\
	ui/lookingGlass.js	\
	ui/magnifier.js		\
	ui/magnifierDBus.js	\
	ui/main.js		\
	ui/messageTray.js	\
	ui/modalDialog.js	\
	ui/sessionMode.js	\
	ui/shellEntry.js	\
	ui/shellMountOperation.js \
	ui/notificationDaemon.js \
	ui/overview.js		\
	ui/panel.js		\
	ui/panelMenu.js		\
	ui/pointerWatcher.js    \
	ui/popupMenu.js		\
	ui/remoteSearch.js	\
	ui/runDialog.js		\
        ui/screenShield.js	\
	ui/scripting.js		\
	ui/search.js		\
	ui/searchDisplay.js	\
	ui/shellDBus.js		\
	ui/status/accessibility.js	\
	ui/status/keyboard.js	\
	ui/status/lockScreenMenu.js	\
	ui/status/network.js	\
	ui/status/power.js	\
	ui/status/volume.js	\
	ui/status/bluetooth.js	\
	ui/tweener.js		\
	ui/unlockDialog.js	\
	ui/userMenu.js		\
	ui/viewSelector.js	\
	ui/wanda.js		\
	ui/windowAttentionHandler.js	\
	ui/windowManager.js	\
	ui/workspace.js		\
	ui/workspaceThumbnail.js	\
	ui/workspacesView.js	\
	ui/workspaceSwitcherPopup.js    \
	ui/xdndHandler.js	\
	ui/components/__init__.js		\
	ui/components/autorunManager.js		\
	ui/components/automountManager.js	\
	ui/components/networkAgent.js		\
	ui/components/polkitAgent.js		\
	ui/components/recorder.js		\
	ui/components/telepathyClient.js	\
	ui/components/keyring.js		\
	$(NULL)

#!/bin/bash

echo "* Relinking libs to private framework versions.."
install_name_tool -change phonon.framework/Versions/4/phonon @executable_path/../Frameworks/phonon.framework/Versions/4/phonon FSMEmulator.app/Contents/MacOS/FSMEmulator
install_name_tool -change QtDBus.framework/Versions/4/QtDBus @executable_path/../Frameworks/QtDBus.framework/Versions/4/QtDBus FSMEmulator.app/Contents/MacOS/FSMEmulator
install_name_tool -change QtXml.framework/Versions/4/QtXml @executable_path/../Frameworks/QtXml.framework/Versions/4/QtXml FSMEmulator.app/Contents/MacOS/FSMEmulator
install_name_tool -change QtGui.framework/Versions/4/QtGui @executable_path/../Frameworks/QtGui.framework/Versions/4/QtGui FSMEmulator.app/Contents/MacOS/FSMEmulator
install_name_tool -change QtCore.framework/Versions/4/QtCore @executable_path/../Frameworks/QtCore.framework/Versions/4/QtCore FSMEmulator.app/Contents/MacOS/FSMEmulator
echo "* .. Done."

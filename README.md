Summary
=======
Retrieves system preferences values set and used by webOS

luna-prefs
==========

This component supports the following methods, which are described in detail in the generated documentation:  

*  com.palm.preferences/appProperties/getAllAppProperties
*  com.palm.preferences/appProperties/getAllAppPropertiesObj
*  com.palm.preferences/appProperties/getAppKeys
*  com.palm.preferences/appProperties/getAppKeysObj
*  com.palm.preferences/appProperties/getAppProperty
*  com.palm.preferences/appProperties/removeAppProperty
*  com.palm.preferences/appProperties/setAppProperty

*  com.palm.preferences/systemProperties/getAllSysProperties
*  com.palm.preferences/systemProperties/getAllSysPropertiesObj
*  com.palm.preferences/systemProperties/getSomeSysProperties
*  com.palm.preferences/systemProperties/getSomeSysPropertiesObj
*  com.palm.preferences/systemProperties/getSysKeys
*  com.palm.preferences/systemProperties/getSysKeysObj
*  com.palm.preferences/systemProperties/getSysProperty


How to Build on Linux
=====================

### Building the latest "stable" version

Clone the repository openwebos/build-desktop and follow the instructions in the README file.

### Building your local clone

First follow the directions to build the latest "stable" version.

To build your local clone of luna-prefs instead of the "stable" version installed with the build-webos-desktop script:  
* Open the build-webos-desktop.sh script with a text editor
* Locate the function build_luna-prefs
* Change the line "cd $BASE/luna-prefs" to use the folder containing your clone, for example "cd ~/github/luna-prefs"
* Close the text editor
* Remove the file ~/luna-desktop-binaries/luna-prefs/luna-desktop-build.stamp
* Start the build

Cautions:
* When you re-clone openwebos/build-desktop, you'll have to overwrite your changes and reapply them
* Components often advance in parallel with each other, so be prepared to keep your cloned repositories updated
* Fetch and rebase frequently

# Copyright and License Information

All content, including all source code files and documentation files in this repository except otherwise noted are: 

 Copyright (c) 2008-2012 Hewlett-Packard Development Company, L.P.

All content, including all source code files and documentation files in this repository except otherwise noted are:
Licensed under the Apache License, Version 2.0 (the "License");
you may not use this content except in compliance with the License.
You may obtain a copy of the License at

http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.


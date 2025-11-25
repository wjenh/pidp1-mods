# Pidp-1 mods updates

This contains files modified from https://github.com/obsolescence/pidp1 to add new functionality.
Note that this may or may not be tracked in the original repo, so apply with care.

# Installing and building

It contains a full build tree.
By default, it will reside in /opt/pidp1-mods but this can be changed.
Look at install/install.sh and change INSTALLDIR as desired.

Make a clean /opt/pidp1-mods (or whatever you set in the script) directory, move into it, check this out there.
Or, just check out the pdp1mods.tar and extract it into your desired directory, then set INSTALLDIR as above.
The default is /opt/pidp1-mods.

Everything after this proceeds as for the original install.

# **NOTE**

Again,the  one checkout difference is that the original had you check out in /opt.
This one you check out in an empty /opt/pidp1-mods directory or other directory of your choice.
Of course, you're probably reading this after checking out in your /opt directory.
Sorry, it's just the way I set up the repo initially, made sense at the time.

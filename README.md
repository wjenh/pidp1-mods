## Pidp-1 mods updates

This contains files modified from https://github.com/obsolescence/pidp1 to add new functionality and fix some issues.
Note that this no longer tracks the original repo since that seems to have been abandoned by the developers.
The link was removed on 3-Feb-26 to make this a fully-independent repository.

It also adds various tools such as the am1 assembler and include files for i, drum utilities, documentation, etc.

## Installing and building

It contains a full build tree.
It is checked out in the /opt directory and will make its own installation directory, which will always be pidp1-mods.

Move to /opt, check this out there:

```
sudo git clone https://github.com/wjenh/pidp1-mods.git
```

Everything after this proceeds as for the original install.

## **NOTE**

You can build some parts without building the entire set.
For example, if you want to use the new assembler, you can just do a make in its directory.
You will need to have flex and bison installed to build it.

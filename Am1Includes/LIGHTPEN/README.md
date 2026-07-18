## Include files for the light pen fpr the Type 30 display

## What's here?

This has the iot definitions for the light pen as well as some useful related macros.

Also included are several utility code includes:
- findPen.ac sweeps a line down the screen looking for the light pen then setting the pen coordinates
- trackPen.ac follows the light pen updating the pen coordinates
- motionFilter.ah and motionFilter.ac provide a sophisicated predictive lightpen tracking filter.
To use it, two filters are initialized, then raw lightpen target-hit coordinates are fed to motionFilterUpdate
for each axis which then returns the value needed to reposition the lightpen target

See the Docs directory for detailed light pen usage.

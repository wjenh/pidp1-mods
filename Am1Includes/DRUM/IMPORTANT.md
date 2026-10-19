# Conflict with the Type 340 display

Both the Type 340 and the Parallel Drum use dra and dss.
If you use both deviced, you will have to use the direct iot values for one or the other or
use the t23dra and t23dss alternate IOT names.
The suggested method is to include type23alt.ah which will define t23dra and t23dss instead of just a bare dra.

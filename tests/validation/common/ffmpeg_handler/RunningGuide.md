# Running tests with FFmpeg handlers

> **Note:** This document is a stub. It is planned to be extended later on.

This document describes how to prepare FFmpeg classes in order to execute a test using FFmpeg handlers.

## Concept

<!-- Table with FFmpegExecutor structure -->
<table border="1px solid">
<tr>
    <td>
        <code>FFmpegExecutor(</code>
        <table border="1px solid">
        <tr>
            <td>
                <code>ff : FFmpeg(</code>
                <table border="1px solid">
                <tr>
                    <td><code>prefix_variables : dict()</code></td>
                    <td><code>ffmpeg_path : str</code></td>
                    <td><code>ffmpeg_input : FFmpegIO()</code></td>
                    <td><code>ffmpeg_output : FFmpegIO()</code></td>
                    <td><code>yes_overwrite : bool</code></td>
                </tr>
                </table>
                <code>)</code>
            </td>
            <td>
                <code>host</code>
            </td>
        </tr>
        </table>
        <code>)</code>
    </td>
</tr>
</table>
<!-- End of table with FFmpegExecutor structure -->

Object of an `FFmpeg` class is directly responsible of building the commands, used by `FFmpegExecutor` and should be treated as an encapsulating element consisting of mainly two components - `ffmpeg_input` and `ffmpeg_output` - both of `FFmpegIO` class or dependent (child class). `prefix_variables` is a dictionary with keys and values that are added at the beginning of the command with
`key=value` (with a space at the end). `ffmpeg_path` is used to determine a path to a specific FFmpeg executable (by default: `ffmpeg` - which means the one available in `$PATH`).

> Notes:
> 1. It is not possible to execute more than a single input and output per instance at the moment. This should be achievable using filters, but they are not implemented yet.
> 2. The child classes to FFmpegIO can be seen on the [FFmpegIO class and subclasses](README.md#ffmpegio-class-and-subclasses) graph.

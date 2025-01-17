# OuterSpatial
This is an implementation of my agent-based economy simulation "[OuterSpatial](https://github.com/halgriffiths/OuterSpatialEngine/)" using SpatialOS.
That project served as a prototype for the mechanics underlying the simulation, but ran locally on a single machine which limited its performance (bottlenecking) and fun (single-player only).

This new project aims to rectify both those issues, and the pipe-dream is for this project to be a fully-playable, persistent MMO game based on trading and idle game mechanics.

#### Speculative road map:
 - [x] Non-spatialOS tick-based prototype
 - [x] Non-spatialOS multithreaded asynchronous prototype
 - [x] Economy mechanics ported to SpatialOS
 - [x] AI behaviour ported and tweaked for SpatialOS
 - [ ] Live terminal plotting
 - [ ] Primitive TUI
 - [ ] Implement human-players with associated UI elements
 - [ ] QA testing begins
 - [ ] Idle-game gameplay elements
## AI traders
An important restriction I wanted to give myself was that the AI actors should have no additional information/assistance other than what human players have. As a result, each AI trader is completely independent and bases all of its decisions entirely on:
 - Its own inventory & liquidity
 - Its own personal understanding of a commodities' "worth"
 - The current price of a commodity
 - It's remembered history of how its previous trades have done

## SpatialOS Quick start

The build scripts (`build.json`) for each worker:

  1. Generate C++ code from schema
  2. Download the C++ worker SDK
  3. Create a `cmake_build` directory in the worker directory
  4. Invoke `cmake` with arguments depending on the target platform

To build and launch a local deployment execute the following commands:

```
spatial worker build
spatial local launch --runtime_version=15.0.0
```

> **Windows users**
>
> If you have [WSL](https://docs.microsoft.com/en-us/windows/wsl/about) installed, it is strongly recommended that you invoke `spatial worker build` directly from a non-WSL bash shell, e.g. git bash. The command executes shell scripts internally, which are likely to fail when invoked from other types of shells (e.g. cmder, powershell).
> Try this if you see the `$'\r': command not found` error, or the `cmake` command not getting found despite being installed.

To connect a new instance of the "External" worker type to a running local deployment (after `spatial local launch`):

```
spatial local worker launch External local
```

## What the project does

When you launch a deployment, a single instance of the `Managed` worker will be started as configured in `default_launch.json`.

The `Managed` worker connects to SpatialOS and then assigns the entity with ID 2 (defined in the snapshot) as a partition entity to itself.
This then gives it authority over the `LoginListenerSet` and `PositionSet` on entity 1, as per the authority delegation already set up on it in the snapshot.

The `Managed` worker updates the position of entity 1 in a loop, making it move around the origin in a circle - this will be visible from the inspector.

The `Interest` component on entity 1 is configured such that the `Managed` worker gains interest in all other entity with the `improbable::restricted::Worker` component.
The `Managed` worker uses this detect when new workers connect to the deployment and log a message to the runtime for each connected worker.

The `External` worker, once manually started, simply connects to SpatialOS and then loops to continually process the Ops list it receives.

## Project structure

The CMake project hierarchy doesn't exactly match the directory structure of
the project. For example the projects for workers add as subdirectories the
`schema` and `dependencies` projects.

This is how projects are structured in the directory:
```
+-- schema/CMakeLists.txt
+-- dependencies/CMakeLists.txt
+-- workers
    |-- External/
    |   |-- External/CMakeLists.txt
    |   |-- CMakeLists.txt
    |   |-- build.json
    |-- Managed/
        |-- Managed/CMakeLists.txt
        |-- CMakeLists.txt
        |-- build.json
```

This enables you to keep the worker directories free of CMake files for schema and dependencies while not needing a CMake file at the root of the project.

The `schema` directory contains a sample empty component called `blank`. It is
not used by the workers directly so feel free to delete it but it's there to
show how sources generated from the schema could be linked in the worker
binary. See `schema/CMakeLists.txt` which creates a library with all generated
sources.

The snapshot exists in both JSON and binary format in the `snapshots` folder. There is no script
to generate the snapshot as the snapshot was written by hand in JSON format, but it's possible
to make simple changes to the JSON snapshot and regenerate the binary snapshot from it. To update the
binary snapshot after making a change, run the following command:

```
spatial project history snapshot convert --input-format=text --input=snapshots/default.json --output-format=binary --output=snapshots/default.snapshot
```

### The worker project

The following applies to both the `Managed` and `External` worker projects but examples will only be about `Managed`.

After running `spatial build` the generated project by CMake will be in
`workers/Managed/cmake_build`. Exactly what it contains will depend on the
generator you use. A worker project includes 3 subdirectories in its
`CMakeLists.txt` - `dependencies`, `schema` and `Managed`. The first two are
not true subdirectories of `workers/Managed` in the file structure but their
binary directories are set as if they were.

On Windows, both the release and debug builds of the Worker SDK are downloaded and set up correctly in
the `CMakeLists.txt`. This means that both the `Release` and `Debug` configurations in the generated
Visual Studio solution (`.sln`) should build and link correctly without any further changes.

## Attaching a debugger

If you use a Visual Studio generator with CMake, the generated solution contains several projects to match the build targets. You can start a worker from Visual Studio by setting the project matching the worker name as the startup project for the solution. It will try to connect to a local deployment by default. You can customize the connection parameters by navigating to `Properties > Configuration properties > Debugging` to set the command arguments. Using `receptionist localhost 7777 DebugWorker` as the command arguments for example will connect a new instance of the worker named `DebugWorker` via the receptionist to a local running deployment. You can do this for both worker types that come with this project. Make sure you are starting the project using a local debugger (e.g. Local Windows Debugger).

## Cloud deployment

Our cloud deployment environment is based on Linux, so therefore if you're not using Linux, you'll
have to set up a cross-compile toolchain and build out a Linux binary (due to the nature of C++).
More information can be found [here](https://docs.improbable.io/reference/latest/cppsdk/building#building-for-a-cloud-deployment).

If using Windows, options include (but are not limited to):
- Install [Ubuntu](https://www.microsoft.com/en-gb/p/ubuntu/9nblggh4msv6) using the [Windows Subsystem for Linux](https://docs.microsoft.com/en-us/windows/wsl/install-win10), then build a Linux worker from within that environment.
- Install Ubuntu in a Virtual Machine with either [VirtualBox](https://www.virtualbox.org/wiki/Downloads) or [VMware Workstation Player](https://www.vmware.com/products/workstation-player/workstation-player-evaluation.html).
- Set up a cross-compiler to be used from Windows, such as the one distributed by Unreal Engine: https://docs.unrealengine.com/en-us/Platforms/Linux/GettingStarted
- Use the Visual Studio 2017 CMake Linux support: https://docs.microsoft.com/en-us/cpp/linux/cmake-linux-project?view=vs-2017

Once this is done and you have successfully built a Linux assembly, set the `project_name` field in
`spatialos.json` to match your SpatialOS project name. Then upload and launch:

```
spatial cloud upload <assembly-name>
spatial cloud launch <assembly-name> default_launch.json <deployment-name> --snapshot=<snapshot-path> --runtime_version=15.0.0
```

See [`spatial cloud connect external`](https://docs.improbable.io/reference/latest/shared/spatial-cli/spatial-cloud-connect-external)
if you want to connect to a cloud deployment. In
addition, the `External` worker has a second external launch configuration
called `cloud` which could be used to connect provided that you know the
deployment name and have a login token:

```
spatial local worker launch External cloud <deployment-name> <login-token>
```

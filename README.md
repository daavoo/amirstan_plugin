# Amirstan_plugin

Amirstan plugin contain some useful tensorrt plugin.
These plugins are used to support some other project such as 

https://github.com/grimoire/torch2trt_dynamic 

https://github.com/grimoire/mmdetection-to-tensorrt


## Requirement

- Tensorrt >= 7.0.0.11
- cub >= 1.8.0

## Installation

config CUB_ROOT_DIR / TENSORRT_ROOT or other library in CMakeLists.txt

```shell
git clone https://github.com/grimoire/amirstan_plugin.git
cd amirstan_plugin
mkdir build
cd build
cmake ..
make -j10
```

set the envoirment variable(in ~/.bashrc):

```shell
export AMIRSTAN_LIBRARY_PATH=<amirstan_plugin_root>/build/lib
```

### DeepStream Support

In order to install this project including support for [DeepStream](https://developer.nvidia.com/deepstream-sdk) you need run:

```shell
git clone https://github.com/grimoire/amirstan_plugin.git
cd amirstan_plugin
mkdir build
cd build
cmake .. -DWITH_DEEPSTREAM=true
make -j10
```

Enabling this option will include  a [custom output parser for deepstream](https://docs.nvidia.com/metropolis/deepstream/dev-guide/index.html#page/DeepStream_Development_Guide/deepstream_custom_model.html#wwpID0E0RB0HA) in the generated shared object library (`libamirstan_plugin.so`).

To be latter referenced in the [DeepStream configuration file](https://docs.nvidia.com/metropolis/deepstream/dev-guide/index.html#page/DeepStream%20Plugins%20Development%20Guide/deepstream_plugin_details.html#wwpID0E04DB0HA) as the following example:

```
parse-bbox-func-name=NvDsInferParseMmdet
output-blob-names=num_detections;boxes;scores;classes
custom-lib-path=/home/nvidia/amirstan_plugin/build/lib/libamirstan_plugin.so
```

You can read more information on how to use it in conjuntion with [`mmdetection-to-tensorrt`](https://github.com/grimoire/mmdetection-to-tensorrt#deepstream)

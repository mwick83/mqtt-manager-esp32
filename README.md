# MqttManager for ESP32

This is an MQTT client management implementation for the ESP32 that uses my [fork of the espmqtt client library](https://github.com/mwick83/espmqtt). It is used to make the client library multi-thread compatible, so it can be used to publish and receive message from different threads.

It is meant to be used as a component within an esp-idf project, i.e. clone it to the components/ dir or add it as a git submodule there. Note that the espmqtt component must also be available.

## License
Copyright (c) 2018 Manuel Wick

Licensed under the BSD 3-clause "New" or "Revised" License.
See LICENSE.md file or [http://www.opensource.org/licenses/BSD-3-Clause](http://www.opensource.org/licenses/BSD-3-Clause) for details.

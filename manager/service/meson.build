# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2023 Intel Corporation

service_conf = configuration_data()
service_conf.set('BINDIR', get_option('prefix') / get_option('bindir'))

# The service files name the MtlManager of this build, so a local prefix works
# the same way as a system one.
system_unit = configure_file(
  input: 'mtl-manager.service.in',
  output: 'mtl-manager.service',
  configuration: service_conf,
)

user_unit = configure_file(
  input: 'mtl-manager-user.service.in',
  output: 'mtl-manager-user.service',
  configuration: service_conf,
)

tmpfiles_rule = files('mtl-manager.conf')

# A systemd directory sits outside the prefix, so `ninja install` leaves it
# alone unless asked. install_service.sh is the usual way in.
if get_option('install_service')
  install_data(system_unit, install_dir: get_option('systemd_system_unit_dir'),
    rename: 'mtl-manager.service')
  install_data(user_unit, install_dir: get_option('systemd_user_unit_dir'),
    rename: 'mtl-manager.service')
  install_data(tmpfiles_rule, install_dir: get_option('tmpfiles_dir'))
endif

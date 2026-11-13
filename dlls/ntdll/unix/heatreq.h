/*
 * GW2 on Mac: the server's requests by name, for heatreq.c's request counts (jdrly/gw2-on-mac#60)
 *
 * The names of enum request in include/wine/server_protocol.h, in its order, as tools/make_requests
 * writes them into server/request_trace.h for the server's trace. ntdll has no such list of its
 * own. heatreq.c turns each into "[REQ_<name>] = <name>", so a name the enum lacks doesn't compile,
 * and checks that the list has as many as the enum. After a change to server/protocol.def, write
 * it again from the enum.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#ifndef __NTDLL_UNIX_HEATREQ_H
#define __NTDLL_UNIX_HEATREQ_H

#define HEAT_REQUESTS(R) \
    R(new_process) R(get_new_process_info) R(new_thread) R(get_startup_info) R(init_process_done) \
    R(init_first_thread) R(init_thread) R(terminate_process) R(terminate_thread) R(get_process_info) \
    R(get_process_debug_info) R(get_process_image_name) R(get_process_vm_counters) R(set_process_info) \
    R(get_thread_info) R(get_thread_times) R(set_thread_info) R(suspend_thread) R(resume_thread) \
    R(queue_apc) R(get_apc_result) R(close_handle) R(set_handle_info) R(dup_handle) \
    R(allocate_reserve_object) R(compare_objects) R(set_object_permanence) R(open_process) \
    R(open_thread) R(select) R(create_event) R(event_op) R(query_event) R(open_event) \
    R(create_keyed_event) R(open_keyed_event) R(create_mutex) R(release_mutex) R(open_mutex) \
    R(query_mutex) R(create_semaphore) R(release_semaphore) R(query_semaphore) R(open_semaphore) \
    R(create_file) R(open_file_object) R(alloc_file_handle) R(get_handle_unix_name) R(get_handle_fd) \
    R(get_directory_cache_entry) R(flush) R(get_file_info) R(get_volume_info) R(lock_file) \
    R(unlock_file) R(recv_socket) R(send_socket) R(socket_get_events) R(socket_send_icmp_id) \
    R(socket_get_icmp_id) R(get_next_console_request) R(read_directory_changes) R(read_change) \
    R(unmount_device) R(create_mapping) R(open_mapping) R(get_mapping_info) R(get_image_map_address) \
    R(map_view) R(map_image_view) R(map_builtin_view) R(get_image_view_info) R(unmap_view) \
    R(get_mapping_committed_range) R(add_mapping_committed_range) R(is_same_mapping) \
    R(get_mapping_filename) R(list_processes) R(create_debug_obj) R(wait_debug_event) \
    R(queue_exception_event) R(get_exception_status) R(continue_debug_event) R(debug_process) \
    R(set_debug_obj_info) R(read_process_memory) R(write_process_memory) R(create_key) R(open_key) \
    R(delete_key) R(flush_key) R(enum_key) R(set_key_value) R(get_key_value) R(enum_key_value) \
    R(delete_key_value) R(load_registry) R(unload_registry) R(save_registry) \
    R(set_registry_notification) R(rename_key) R(create_timer) R(open_timer) R(set_timer) \
    R(cancel_timer) R(get_timer_info) R(get_thread_context) R(set_thread_context) R(add_atom) \
    R(delete_atom) R(find_atom) R(get_atom_information) R(add_user_atom) R(get_user_atom_name) \
    R(get_msg_queue_handle) R(get_msg_queue) R(set_queue_fd) R(set_queue_mask) R(get_queue_status) \
    R(get_process_idle_event) R(send_message) R(post_quit_message) R(send_hardware_message) \
    R(get_message) R(reply_message) R(accept_hardware_message) R(get_message_reply) R(set_win_timer) \
    R(kill_win_timer) R(is_window_hung) R(get_serial_info) R(set_serial_info) R(cancel_sync) \
    R(register_async) R(cancel_async) R(get_async_result) R(set_async_direct_result) R(read) R(write) \
    R(ioctl) R(set_irp_result) R(create_named_pipe) R(set_named_pipe_info) R(create_window) \
    R(destroy_window) R(get_desktop_window) R(set_window_owner) R(get_window_info) R(init_window_info) \
    R(set_window_info) R(set_window_fnid) R(set_parent) R(get_window_parents) R(get_window_list) \
    R(get_class_windows) R(get_window_children_from_point) R(get_window_tree) R(set_window_pos) \
    R(get_window_rectangles) R(get_window_text) R(set_window_text) R(get_windows_offset) \
    R(get_visible_region) R(get_window_region) R(set_window_region) R(get_update_region) \
    R(update_window_zorder) R(redraw_window) R(set_window_property) R(remove_window_property) \
    R(get_window_property) R(get_window_properties) R(create_winstation) R(open_winstation) \
    R(close_winstation) R(set_winstation_monitors) R(get_process_winstation) R(set_process_winstation) \
    R(enum_winstation) R(create_desktop) R(open_desktop) R(open_input_desktop) R(set_input_desktop) \
    R(close_desktop) R(get_thread_desktop) R(set_thread_desktop) R(set_user_object_info) \
    R(register_hotkey) R(unregister_hotkey) R(attach_thread_input) R(get_thread_input) \
    R(set_user_input_time) R(get_key_state) R(set_key_state) R(set_foreground_window) \
    R(set_focus_window) R(set_active_window) R(set_capture_window) R(set_caret_window) R(set_caret_info) \
    R(set_hook) R(remove_hook) R(start_hook_chain) R(finish_hook_chain) R(get_hook_info) R(create_class) \
    R(destroy_class) R(set_class_info) R(get_class_info) R(open_clipboard) R(close_clipboard) \
    R(empty_clipboard) R(set_clipboard_data) R(get_clipboard_data) R(get_clipboard_formats) \
    R(enum_clipboard_formats) R(release_clipboard) R(get_clipboard_info) R(set_clipboard_viewer) \
    R(add_clipboard_listener) R(remove_clipboard_listener) R(create_token) R(open_token) \
    R(set_desktop_shell_windows) R(adjust_token_privileges) R(get_token_privileges) \
    R(check_token_privileges) R(duplicate_token) R(filter_token) R(access_check) R(get_token_sid) \
    R(get_token_groups) R(get_token_default_dacl) R(set_token_default_dacl) R(set_security_object) \
    R(get_security_object) R(get_system_handles) R(get_tcp_connections) R(get_udp_endpoints) \
    R(create_mailslot) R(set_mailslot_info) R(create_directory) R(open_directory) \
    R(get_directory_entries) R(create_symlink) R(open_symlink) R(query_symlink) R(get_object_info) \
    R(get_object_name) R(get_object_type) R(get_object_types) R(allocate_locally_unique_id) \
    R(create_device_manager) R(create_device) R(delete_device) R(get_next_device_request) \
    R(get_kernel_object_ptr) R(set_kernel_object_ptr) R(grab_kernel_object) R(release_kernel_object) \
    R(get_kernel_object_handle) R(make_process_system) R(grant_process_admin_token) R(get_token_info) \
    R(create_linked_token) R(create_completion) R(open_completion) R(add_completion) \
    R(remove_completion) R(get_thread_completion) R(query_completion) R(set_completion_info) \
    R(add_fd_completion) R(set_fd_completion_mode) R(set_fd_disp_info) R(set_fd_name_info) \
    R(set_fd_eof_info) R(get_window_layered_info) R(set_window_layered_info) R(alloc_user_handle) \
    R(free_user_handle) R(set_cursor) R(get_cursor_history) R(get_rawinput_buffer) \
    R(update_rawinput_devices) R(create_job) R(open_job) R(assign_job) R(process_in_job) \
    R(set_job_limits) R(set_job_completion_port) R(get_job_info) R(terminate_job) R(suspend_process) \
    R(resume_process) R(get_next_process) R(get_next_thread) R(set_keyboard_repeat) \
    R(get_inproc_sync_fd) R(get_inproc_alert_fd) R(d3dkmt_object_create) R(d3dkmt_object_update) \
    R(d3dkmt_object_query) R(d3dkmt_object_open) R(d3dkmt_share_objects) R(d3dkmt_object_open_name) \
    R(d3dkmt_mutex_acquire) R(d3dkmt_mutex_release) R(d3dkmt_sync_wait) R(d3dkmt_sync_signal) \
    R(alpc_create_port)

#endif /* __NTDLL_UNIX_HEATREQ_H */

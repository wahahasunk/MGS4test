#include "stdafx.h"
#include "RSXThread.h"

#include "Emu/Cell/PPUCallback.h"
#include "Emu/Cell/timers.hpp"

#include "Common/BufferUtils.h"
#include "Common/buffer_stream.hpp"
#include "Common/texture_cache.h"
#include "Common/surface_store.h"
#include "Common/time.hpp"
#include "Capture/rsx_capture.h"
#include "rsx_methods.h"
#include "gcm_printing.h"
#include "RSXDisAsm.h"
#include "Emu/Cell/lv2/sys_event.h"
#include "Emu/Cell/lv2/sys_time.h"
#include "Emu/Cell/Modules/cellGcmSys.h"
#include "Overlays/overlay_perf_metrics.h"
#include "Program/GLSLCommon.h"
#include "Utilities/date_time.h"
#include "Utilities/StrUtil.h"

#include "util/serialization.hpp"
#include "util/asm.hpp"

#include <span>
#include <sstream>
#include <thread>
#include <unordered_set>
#include <cfenv>

class GSRender;

#define CMD_DEBUG 0

atomic_t<bool> g_user_asked_for_frame_capture = false;
atomic_t<bool> g_disable_frame_limit = false;
rsx::frame_trace_data frame_debug;
rsx::frame_capture_data frame_capture;

extern CellGcmOffsetTable offsetTable;
extern thread_local std::string(*g_tls_log_prefix)();

template <>
bool serialize<rsx::rsx_state>(utils::serial& ar, rsx::rsx_state& o)
{
	return ar(o.transform_program, /*o.transform_constants,*/ o.registers);
}

template <>
bool serialize<rsx::frame_capture_data>(utils::serial& ar, rsx::frame_capture_data& o)
{
	ar(o.magic, o.version, o.LE_format);

	if (o.magic != rsx::c_fc_magic || o.version != rsx::c_fc_version || o.LE_format != u32{std::endian::little == std::endian::native})
	{
		return false;
	}

	return ar(o.tile_map, o.memory_map, o.memory_data_map, o.display_buffers_map, o.replay_commands, o.reg_state);
}

template <>
bool serialize<rsx::frame_capture_data::memory_block_data>(utils::serial& ar, rsx::frame_capture_data::memory_block_data& o)
{
	return ar(o.data);
}

template <>
bool serialize<rsx::frame_capture_data::replay_command>(utils::serial& ar, rsx::frame_capture_data::replay_command& o)
{
	return ar(o.rsx_command, o.memory_state, o.tile_state, o.display_buffer_state);
}

namespace rsx
{
	std::function<bool(u32 addr, bool is_writing)> g_access_violation_handler;

	u32 get_address(u32 offset, u32 location, u32 size_to_check, u32 line, u32 col, const char* file, const char* func)
	{
		const auto render = get_current_renderer();
		std::string_view msg;

		switch (location)
		{
		case CELL_GCM_CONTEXT_DMA_MEMORY_FRAME_BUFFER:
		case CELL_GCM_LOCATION_LOCAL:
		{
			if (offset < render->local_mem_size && render->local_mem_size - offset >= size_to_check)
			{
				return rsx::constants::local_mem_base + offset;
			}

			msg = "Local RSX offset out of range!"sv;
			break;
		}

		case CELL_GCM_CONTEXT_DMA_MEMORY_HOST_BUFFER:
		case CELL_GCM_LOCATION_MAIN:
		{
			if (const u32 ea = render->iomap_table.get_addr(offset); ea + 1)
			{
				if (!size_to_check || vm::check_addr(ea, 0, size_to_check))
				{
					return ea;
				}
			}

			msg = "RSXIO memory not mapped!"sv;
			break;
		}

		case CELL_GCM_CONTEXT_DMA_REPORT_LOCATION_LOCAL:
		{
			if (offset < sizeof(RsxReports::report) /*&& (offset % 0x10) == 0*/)
			{
				return render->label_addr + ::offset32(&RsxReports::report) + offset;
			}

			msg = "Local RSX REPORT offset out of range!"sv;
			break;
		}

		case CELL_GCM_CONTEXT_DMA_REPORT_LOCATION_MAIN:
		{
			if (const u32 ea = offset < 0x1000000 ? render->iomap_table.get_addr(0x0e000000 + offset) : -1; ea + 1)
			{
				if (!size_to_check || vm::check_addr(ea, 0, size_to_check))
				{
					return ea;
				}
			}

			msg = "RSXIO REPORT memory not mapped!"sv;
			break;
		}

		// They are handled elsewhere for targeted methods, so it's unexpected for them to be passed here
		case CELL_GCM_CONTEXT_DMA_TO_MEMORY_GET_NOTIFY0:
		case CELL_GCM_CONTEXT_DMA_TO_MEMORY_GET_NOTIFY1:
		case CELL_GCM_CONTEXT_DMA_TO_MEMORY_GET_NOTIFY2:
		case CELL_GCM_CONTEXT_DMA_TO_MEMORY_GET_NOTIFY3:
		case CELL_GCM_CONTEXT_DMA_TO_MEMORY_GET_NOTIFY4:
		case CELL_GCM_CONTEXT_DMA_TO_MEMORY_GET_NOTIFY5:
		case CELL_GCM_CONTEXT_DMA_TO_MEMORY_GET_NOTIFY6:
		case CELL_GCM_CONTEXT_DMA_TO_MEMORY_GET_NOTIFY7:
			msg = "CELL_GCM_CONTEXT_DMA_TO_MEMORY_GET_NOTIFYx"sv; break;

		case CELL_GCM_CONTEXT_DMA_NOTIFY_MAIN_0:
		case CELL_GCM_CONTEXT_DMA_NOTIFY_MAIN_1:
		case CELL_GCM_CONTEXT_DMA_NOTIFY_MAIN_2:
		case CELL_GCM_CONTEXT_DMA_NOTIFY_MAIN_3:
		case CELL_GCM_CONTEXT_DMA_NOTIFY_MAIN_4:
		case CELL_GCM_CONTEXT_DMA_NOTIFY_MAIN_5:
		case CELL_GCM_CONTEXT_DMA_NOTIFY_MAIN_6:
		case CELL_GCM_CONTEXT_DMA_NOTIFY_MAIN_7:
			msg = "CELL_GCM_CONTEXT_DMA_NOTIFY_MAIN_x"sv; break;

		case CELL_GCM_CONTEXT_DMA_SEMAPHORE_RW:
		case CELL_GCM_CONTEXT_DMA_SEMAPHORE_R:
		{
			if (offset < sizeof(RsxReports::semaphore) /*&& (offset % 0x10) == 0*/)
			{
				return render->label_addr + offset;
			}

			msg = "DMA SEMAPHORE offset out of range!"sv;
			break;
		}

		case CELL_GCM_CONTEXT_DMA_DEVICE_RW:
		case CELL_GCM_CONTEXT_DMA_DEVICE_R:
		{
			if (offset < 0x100000 /*&& (offset % 0x10) == 0*/)
			{
				return render->device_addr + offset;
			}

			// TODO: What happens here? It could wrap around or access other segments of rsx internal memory etc
			// Or can simply throw access violation error
			msg = "DMA DEVICE offset out of range!"sv;
			break;
		}

		default:
		{
			msg = "Invalid location!"sv;
			break;
		}
		}

		if (size_to_check)
		{
			// Allow failure if specified size
			// This is to allow accurate recovery for failures
			rsx_log.warning("rsx::get_address(offset=0x%x, location=0x%x, size=0x%x): %s%s", offset, location, size_to_check, msg, src_loc{line, col, file, func});
			return 0;
		}

		fmt::throw_exception("rsx::get_address(offset=0x%x, location=0x%x): %s%s", offset, location, msg, src_loc{line, col, file, func});
	}

	std::pair<u32, u32> interleaved_range_info::calculate_required_range(u32 first, u32 count) const
	{
		if (single_vertex)
		{
			return { 0, 1 };
		}

		const u32 max_index = (first + count) - 1;
		u32 _max_index = 0;
		u32 _min_index = first;

		for (const auto &attrib : locations)
		{
			if (attrib.frequency <= 1) [[likely]]
			{
				_max_index = max_index;
			}
			else
			{
				if (attrib.modulo)
				{
					if (max_index >= attrib.frequency)
					{
						// Actually uses the modulo operator
						_min_index = 0;
						_max_index = attrib.frequency - 1;
					}
					else
					{
						// Same as having no modulo
						_max_index = max_index;
					}
				}
				else
				{
					// Division operator
					_min_index = std::min(_min_index, first / attrib.frequency);
					_max_index = std::max<u32>(_max_index, utils::aligned_div(max_index, attrib.frequency));
				}
			}
		}

		ensure(_max_index >= _min_index);
		return { _min_index, (_max_index - _min_index) + 1 };
	}

	u32 get_vertex_type_size_on_host(vertex_base_type type, u32 size)
	{
		switch (type)
		{
		case vertex_base_type::s1:
		case vertex_base_type::s32k:
			switch (size)
			{
			case 1:
			case 2:
			case 4:
				return sizeof(u16) * size;
			case 3:
				return sizeof(u16) * 4;
			default:
				break;
			}
			fmt::throw_exception("Wrong vector size");
		case vertex_base_type::f: return sizeof(f32) * size;
		case vertex_base_type::sf:
			switch (size)
			{
			case 1:
			case 2:
			case 4:
				return sizeof(f16) * size;
			case 3:
				return sizeof(f16) * 4;
			default:
				break;
			}
			fmt::throw_exception("Wrong vector size");
		case vertex_base_type::ub:
			switch (size)
			{
			case 1:
			case 2:
			case 4:
				return sizeof(u8) * size;
			case 3:
				return sizeof(u8) * 4;
			default:
				break;
			}
			fmt::throw_exception("Wrong vector size");
		case vertex_base_type::cmp: return 4;
		case vertex_base_type::ub256: ensure(size == 4); return sizeof(u8) * 4;
		default:
			break;
		}
		fmt::throw_exception("RSXVertexData::GetTypeSize: Bad vertex data type (%d)!", static_cast<u8>(type));
	}

	void tiled_region::write(const void *src, u32 width, u32 height, u32 pitch)
	{
		if (!tile)
		{
			memcpy(ptr, src, height * pitch);
			return;
		}

		u32 offset_x = base % tile->pitch;
		u32 offset_y = base / tile->pitch;

		switch (tile->comp)
		{
		case CELL_GCM_COMPMODE_C32_2X1:
		case CELL_GCM_COMPMODE_DISABLED:
			for (u32 y = 0; y < height; ++y)
			{
				memcpy(ptr + (offset_y + y) * tile->pitch + offset_x, static_cast<const u8*>(src) + pitch * y, pitch);
			}
			break;
			/*
		case CELL_GCM_COMPMODE_C32_2X1:
			for (u32 y = 0; y < height; ++y)
			{
				for (u32 x = 0; x < width; ++x)
				{
					u32 value = *(u32*)((u8*)src + pitch * y + x * sizeof(u32));

					*(u32*)(ptr + (offset_y + y) * tile->pitch + offset_x + (x * 2 + 0) * sizeof(u32)) = value;
					*(u32*)(ptr + (offset_y + y) * tile->pitch + offset_x + (x * 2 + 1) * sizeof(u32)) = value;
				}
			}
			break;
			*/
		case CELL_GCM_COMPMODE_C32_2X2:
			for (u32 y = 0; y < height; ++y)
			{
				for (u32 x = 0; x < width; ++x)
				{
					u32 value = *reinterpret_cast<const u32*>(static_cast<const u8*>(src) + pitch * y + x * sizeof(u32));

					*reinterpret_cast<u32*>(ptr + (offset_y + y * 2 + 0) * tile->pitch + offset_x + (x * 2 + 0) * sizeof(u32)) = value;
					*reinterpret_cast<u32*>(ptr + (offset_y + y * 2 + 0) * tile->pitch + offset_x + (x * 2 + 1) * sizeof(u32)) = value;
					*reinterpret_cast<u32*>(ptr + (offset_y + y * 2 + 1) * tile->pitch + offset_x + (x * 2 + 0) * sizeof(u32)) = value;
					*reinterpret_cast<u32*>(ptr + (offset_y + y * 2 + 1) * tile->pitch + offset_x + (x * 2 + 1) * sizeof(u32)) = value;
				}
			}
			break;
		default:
			::narrow(tile->comp);
		}
	}

	void tiled_region::read(void *dst, u32 width, u32 height, u32 pitch)
	{
		if (!tile)
		{
			memcpy(dst, ptr, height * pitch);
			return;
		}

		u32 offset_x = base % tile->pitch;
		u32 offset_y = base / tile->pitch;

		switch (tile->comp)
		{
		case CELL_GCM_COMPMODE_C32_2X1:
		case CELL_GCM_COMPMODE_DISABLED:
			for (u32 y = 0; y < height; ++y)
			{
				memcpy(static_cast<u8*>(dst) + pitch * y, ptr + (offset_y + y) * tile->pitch + offset_x, pitch);
			}
			break;
			/*
		case CELL_GCM_COMPMODE_C32_2X1:
			for (u32 y = 0; y < height; ++y)
			{
				for (u32 x = 0; x < width; ++x)
				{
					u32 value = *(u32*)(ptr + (offset_y + y) * tile->pitch + offset_x + (x * 2 + 0) * sizeof(u32));

					*(u32*)((u8*)dst + pitch * y + x * sizeof(u32)) = value;
				}
			}
			break;
			*/
		case CELL_GCM_COMPMODE_C32_2X2:
			for (u32 y = 0; y < height; ++y)
			{
				for (u32 x = 0; x < width; ++x)
				{
					u32 value = *reinterpret_cast<u32*>(ptr + (offset_y + y * 2 + 0) * tile->pitch + offset_x + (x * 2 + 0) * sizeof(u32));

					*reinterpret_cast<u32*>(static_cast<u8*>(dst) + pitch * y + x * sizeof(u32)) = value;
				}
			}
			break;
		default:
			::narrow(tile->comp);
		}
	}

	thread::~thread()
	{
		g_access_violation_handler = nullptr;
	}

	thread::thread()
		: cpu_thread(0x5555'5555)
	{
		g_access_violation_handler = [this](u32 address, bool is_writing)
		{
			return on_access_violation(address, is_writing);
		};

		m_rtts_dirty = true;
		m_textures_dirty.fill(true);
		m_vertex_textures_dirty.fill(true);

		m_graphics_state = pipeline_state::all_dirty;

		g_user_asked_for_frame_capture = false;

		if (g_cfg.misc.use_native_interface && (g_cfg.video.renderer == video_renderer::opengl || g_cfg.video.renderer == video_renderer::vulkan))
		{
			m_overlay_manager = g_fxo->init<rsx::overlays::display_manager>(0);
		}

		state -= cpu_flag::stop + cpu_flag::wait; // TODO: Remove workaround
	}

	void thread::capture_frame(const std::string &name)
	{
		frame_trace_data::draw_state draw_state{};

		draw_state.programs = get_programs();
		draw_state.name = name;
		frame_debug.draw_calls.emplace_back(std::move(draw_state));
	}

	void thread::begin()
	{
		if (cond_render_ctrl.hw_cond_active)
		{
			if (!cond_render_ctrl.eval_pending())
			{
				// End conditional rendering if still active
				end_conditional_rendering();
			}

			// If hw cond render is enabled and evalutation is still pending, do nothing
		}
		else if (cond_render_ctrl.eval_pending())
		{
			// Evaluate conditional rendering test or enable hw cond render until results are available
			if (backend_config.supports_hw_conditional_render)
			{
				// In this mode, it is possible to skip the cond render while the backend is still processing data.
				// The backend guarantees that any draw calls emitted during this time will NOT generate any ROP writes
				ensure(!cond_render_ctrl.hw_cond_active);

				// Pending evaluation, use hardware test
				begin_conditional_rendering(cond_render_ctrl.eval_sources);
			}
			else
			{
				// NOTE: eval_sources list is reversed with newest query first
				zcull_ctrl->read_barrier(this, cond_render_ctrl.eval_address, cond_render_ctrl.eval_sources.front());
				ensure(!cond_render_ctrl.eval_pending());
			}
		}

		in_begin_end = true;
	}

	void thread::append_to_push_buffer(u32 attribute, u32 size, u32 subreg_index, vertex_base_type type, u32 value)
	{
		if (!(rsx::method_registers.vertex_attrib_input_mask() & (1 << attribute)))
		{
			return;
		}

		// Enforce ATTR0 as vertex attribute for push buffers.
		// This whole thing becomes a mess if we don't have a provoking attribute.
		const auto vertex_id = vertex_push_buffers[0].get_vertex_id();
		vertex_push_buffers[attribute].set_vertex_data(attribute, vertex_id, subreg_index, type, size, value);
		m_graphics_state |= rsx::pipeline_state::push_buffer_arrays_dirty;
	}

	u32 thread::get_push_buffer_vertex_count() const
	{
		// Enforce ATTR0 as vertex attribute for push buffers.
		// This whole thing becomes a mess if we don't have a provoking attribute.
		return vertex_push_buffers[0].vertex_count;
	}

	void thread::append_array_element(u32 index)
	{
		// Endianness is swapped because common upload code expects input in BE
		// TODO: Implement fast upload path for LE inputs and do away with this
		element_push_buffer.push_back(std::bit_cast<u32, be_t<u32>>(index));
	}

	u32 thread::get_push_buffer_index_count() const
	{
		return ::size32(element_push_buffer);
	}

	void thread::end()
	{
		if (capture_current_frame)
		{
			capture::capture_draw_memory(this);
		}

		in_begin_end = false;
		m_frame_stats.draw_calls++;

		method_registers.current_draw_clause.post_execute_cleanup();

		m_graphics_state |= rsx::pipeline_state::framebuffer_reads_dirty;
		m_eng_interrupt_mask |= rsx::backend_interrupt;
		ROP_sync_timestamp = rsx::get_shared_tag();

		if (m_graphics_state & rsx::pipeline_state::push_buffer_arrays_dirty)
		{
			for (auto& push_buf : vertex_push_buffers)
			{
				//Disabled, see https://github.com/RPCS3/rpcs3/issues/1932
				//rsx::method_registers.register_vertex_info[index].size = 0;

				push_buf.clear();
			}

			m_graphics_state &= ~rsx::pipeline_state::push_buffer_arrays_dirty;
		}

		element_push_buffer.clear();

		zcull_ctrl->on_draw();

		if (capture_current_frame)
		{
			u32 element_count = rsx::method_registers.current_draw_clause.get_elements_count();
			capture_frame(fmt::format("Draw %s %d", rsx::method_registers.current_draw_clause.primitive, element_count));
		}
	}

	void thread::execute_nop_draw()
	{
		method_registers.current_draw_clause.begin();
		do
		{
			method_registers.current_draw_clause.execute_pipeline_dependencies();
		}
		while (method_registers.current_draw_clause.next());
	}

	void thread::cpu_task()
	{
		while (Emu.IsReady())
		{
			thread_ctrl::wait_for(1000);
		}

		on_task();
		on_exit();
	}

	void thread::cpu_wait(bs_t<cpu_flag> old)
	{
		if (external_interrupt_lock)
		{
			wait_pause();
		}

		on_semaphore_acquire_wait();

		if ((state & (cpu_flag::dbg_global_pause + cpu_flag::exit)) == cpu_flag::dbg_global_pause)
		{
			// Wait 16ms during emulation pause. This reduces cpu load while still giving us the chance to render overlays.
			thread_ctrl::wait_on(state, old, 16000);
		}
		else
		{
			std::this_thread::yield();
		}
	}

	void thread::on_task()
	{
		g_tls_log_prefix = []
		{
			const auto rsx = get_current_renderer();
			return fmt::format("RSX [0x%07x]", rsx->ctrl ? +rsx->ctrl->get : 0);
		};

		method_registers.init();

		rsx::overlays::reset_performance_overlay();

		g_fxo->get<rsx::dma_manager>().init();
		on_init_thread();

		is_inited = true;
		is_inited.notify_all();

		if (!zcull_ctrl)
		{
			//Backend did not provide an implementation, provide NULL object
			zcull_ctrl = std::make_unique<::rsx::reports::ZCULL_control>();
		}

		performance_counters.state = FIFO_state::empty;

		// Wait for startup (TODO)
		while (m_rsx_thread_exiting)
		{
			// Wait for external pause events
			if (external_interrupt_lock)
			{
				wait_pause();
			}

			// Execute backend-local tasks first
			do_local_task(performance_counters.state);

			// Update sub-units
			zcull_ctrl->update(this);

			if (is_stopped())
			{
				return;
			}

			thread_ctrl::wait_for(1000);
		}

		performance_counters.state = FIFO_state::running;

		fifo_ctrl = std::make_unique<::rsx::FIFO::FIFO_control>(this);

		last_guest_flip_timestamp = rsx::uclock() - 1000000;

		vblank_count = 0;

		g_fxo->init<named_thread>("VBlank Thread", [this]()
		{
			// See sys_timer_usleep for details
#ifdef __linux__
			constexpr u32 host_min_quantum = 50;
#else
			constexpr u32 host_min_quantum = 500;
#endif
			u64 start_time = get_system_time();

			u64 vblank_rate = g_cfg.video.vblank_rate;
			u64 vblank_period = 1'000'000 + u64{g_cfg.video.vblank_ntsc.get()} * 1000;

			u64 local_vblank_count = 0;

			// TODO: exit condition
			while (!is_stopped())
			{
				// Get current time
				const u64 current = get_system_time();

				// Calculate the time at which we need to send a new VBLANK signal
				const u64 post_event_time = start_time + (local_vblank_count + 1) * vblank_period / vblank_rate;

				// Calculate time remaining to that time (0 if we passed it)
				const u64 wait_for = current >= post_event_time ? 0 : post_event_time - current;

				// Substract host operating system min sleep quantom to get sleep time
				const u64 wait_sleep = wait_for - u64{wait_for >= host_min_quantum} * host_min_quantum;

				if (!wait_for)
				{
					{
						local_vblank_count++;
						vblank_count++;

						if (local_vblank_count == vblank_rate)
						{
							// Advance start_time to the moment of the current VBLANK
							// Which is the last VBLANK event in this period
							// This is in order for multiplication by ratio above to use only small numbers
							start_time += vblank_period;
							local_vblank_count = 0;

							// We have a rare chance to update settings without losing precision whenever local_vblank_count is 0
							vblank_rate = g_cfg.video.vblank_rate;
							vblank_period = 1'000'000 + u64{g_cfg.video.vblank_ntsc.get()} * 1000;
						}
	
						if (isHLE)
						{
							if (vblank_handler)
							{
								intr_thread->cmd_list
								({
									{ ppu_cmd::set_args, 1 }, u64{1},
									{ ppu_cmd::lle_call, vblank_handler },
									{ ppu_cmd::sleep, 0 }
								});

								intr_thread->cmd_notify.notify_one();
							}
						}
						else
						{
							sys_rsx_context_attribute(0x55555555, 0xFED, 1, get_guest_system_time(post_event_time), 0, 0);
						}
					}
				}
				else if (wait_sleep)
				{
					thread_ctrl::wait_for(wait_sleep);
				}
				else if (wait_for >= host_min_quantum / 3 * 2)
				{
					std::this_thread::yield();
				}

				if (Emu.IsPaused())
				{
					// Save the difference before pause
					start_time = rsx::uclock() - start_time;

					while (Emu.IsPaused() && !is_stopped())
					{
						thread_ctrl::wait_for(5'000);
					}

					// Restore difference
					start_time = rsx::uclock() - start_time;
				}
			}
		});

		// Raise priority above other threads
		thread_ctrl::scoped_priority high_prio(+1);

		if (g_cfg.core.thread_scheduler != thread_scheduler_mode::os)
		{
			thread_ctrl::set_thread_affinity_mask(thread_ctrl::get_affinity_mask(thread_class::rsx));
		}

		while (!test_stopped())
		{
			// Wait for external pause events
			if (external_interrupt_lock)
			{
				wait_pause();
			}

			// Note a possible rollback address
			if (sync_point_request && !in_begin_end)
			{
				restore_point = ctrl->get;
				saved_fifo_ret = fifo_ret_addr;
				sync_point_request.release(false);
			}

			// Update sub-units every 64 cycles. The local handler is invoked for other functions externally on-demand anyway.
			// This avoids expensive calls to check timestamps which involves reading some values from TLS storage on windows.
			// If something is going on in the backend that requires an update, set the interrupt bit explicitly.
			if ((m_cycles_counter++ & 63) == 0 || m_eng_interrupt_mask)
			{
				// Execute backend-local tasks first
				do_local_task(performance_counters.state);

				// Update other sub-units
				zcull_ctrl->update(this);
			}

			// Execute FIFO queue
			run_FIFO();
		}
	}

	void thread::on_exit()
	{
		// Deregister violation handler
		g_access_violation_handler = nullptr;

		// Clear any pending flush requests to release threads
		std::this_thread::sleep_for(1ms);
		do_local_task(rsx::FIFO_state::lock_wait);

		m_rsx_thread_exiting = true;
		g_fxo->get<rsx::dma_manager>().join();
		state += cpu_flag::exit;
	}

	void thread::fill_scale_offset_data(void *buffer, bool flip_y) const
	{
		int clip_w = rsx::method_registers.surface_clip_width();
		int clip_h = rsx::method_registers.surface_clip_height();

		float scale_x = rsx::method_registers.viewport_scale_x() / (clip_w / 2.f);
		float offset_x = rsx::method_registers.viewport_offset_x() - (clip_w / 2.f);
		offset_x /= clip_w / 2.f;

		float scale_y = rsx::method_registers.viewport_scale_y() / (clip_h / 2.f);
		float offset_y = (rsx::method_registers.viewport_offset_y() - (clip_h / 2.f));
		offset_y /= clip_h / 2.f;
		if (flip_y) scale_y *= -1;
		if (flip_y) offset_y *= -1;

		float scale_z = rsx::method_registers.viewport_scale_z();
		float offset_z = rsx::method_registers.viewport_offset_z();
		float one = 1.f;

		utils::stream_vector(buffer, std::bit_cast<u32>(scale_x), 0, 0, std::bit_cast<u32>(offset_x));
		utils::stream_vector(static_cast<char*>(buffer) + 16, 0, std::bit_cast<u32>(scale_y), 0, std::bit_cast<u32>(offset_y));
		utils::stream_vector(static_cast<char*>(buffer) + 32, 0, 0, std::bit_cast<u32>(scale_z), std::bit_cast<u32>(offset_z));
		utils::stream_vector(static_cast<char*>(buffer) + 48, 0, 0, 0, std::bit_cast<u32>(one));
	}

	void thread::fill_user_clip_data(void *buffer) const
	{
		const rsx::user_clip_plane_op clip_plane_control[6] =
		{
			rsx::method_registers.clip_plane_0_enabled(),
			rsx::method_registers.clip_plane_1_enabled(),
			rsx::method_registers.clip_plane_2_enabled(),
			rsx::method_registers.clip_plane_3_enabled(),
			rsx::method_registers.clip_plane_4_enabled(),
			rsx::method_registers.clip_plane_5_enabled(),
		};

		u8 data_block[64];
		s32* clip_enabled_flags = reinterpret_cast<s32*>(data_block);
		f32* clip_distance_factors = reinterpret_cast<f32*>(data_block + 32);

		for (int index = 0; index < 6; ++index)
		{
			switch (clip_plane_control[index])
			{
			default:
				rsx_log.error("bad clip plane control (0x%x)", static_cast<u8>(clip_plane_control[index]));
				[[fallthrough]];

			case rsx::user_clip_plane_op::disable:
				clip_enabled_flags[index] = 0;
				clip_distance_factors[index] = 0.f;
				break;

			case rsx::user_clip_plane_op::greater_or_equal:
				clip_enabled_flags[index] = 1;
				clip_distance_factors[index] = 1.f;
				break;

			case rsx::user_clip_plane_op::less_than:
				clip_enabled_flags[index] = 1;
				clip_distance_factors[index] = -1.f;
				break;
			}
		}

		memcpy(buffer, data_block, 2 * 8 * sizeof(u32));
	}

	/**
	* Fill buffer with vertex program constants.
	* Buffer must be at least 512 float4 wide.
	*/
	void thread::fill_vertex_program_constants_data(void* buffer, const std::vector<u16>& reloc_table)
	{
		if (!reloc_table.empty()) [[ likely ]]
		{
			char* dst = reinterpret_cast<char*>(buffer);
			for (const auto& index : reloc_table)
			{
				utils::stream_vector_from_memory(dst, &rsx::method_registers.transform_constants[index]);
				dst += 16;
			}
		}
		else
		{
			memcpy(buffer, rsx::method_registers.transform_constants.data(), 468 * 4 * sizeof(float));
		}
	}

	void thread::fill_fragment_state_buffer(void* buffer, const RSXFragmentProgram& /*fragment_program*/)
	{
		u32 rop_control = 0u;

		if (rsx::method_registers.alpha_test_enabled())
		{
			const u32 alpha_func = static_cast<u32>(rsx::method_registers.alpha_func());
			rop_control |= (alpha_func << 16);
			rop_control |= ROP_control::alpha_test_enable;
		}

		if (rsx::method_registers.polygon_stipple_enabled())
		{
			rop_control |= ROP_control::polygon_stipple_enable;
		}

		if (rsx::method_registers.msaa_alpha_to_coverage_enabled() && !backend_config.supports_hw_a2c)
		{
			// TODO: Properly support alpha-to-coverage and alpha-to-one behavior in shaders
			// Alpha values generate a coverage mask for order independent blending
			// Requires hardware AA to work properly (or just fragment sample stage in fragment shaders)
			// Simulated using combined alpha blend and alpha test
			if (rsx::method_registers.msaa_sample_mask()) rop_control |= ROP_control::msaa_mask_enable;
			rop_control |= ROP_control::csaa_enable;

			// Sample configuration bits
			switch (rsx::method_registers.surface_antialias())
			{
				case rsx::surface_antialiasing::center_1_sample:
					break;
				case rsx::surface_antialiasing::diagonal_centered_2_samples:
					rop_control |= 1u << 6;
					break;
				default:
					rop_control |= 3u << 6;
					break;
			}
		}

		const f32 fog0 = rsx::method_registers.fog_params_0();
		const f32 fog1 = rsx::method_registers.fog_params_1();
		const u32 fog_mode = static_cast<u32>(rsx::method_registers.fog_equation());

		if (rsx::method_registers.framebuffer_srgb_enabled())
		{
			// Check if framebuffer is actually an XRGB format and not a WZYX format
			switch (rsx::method_registers.surface_color())
			{
			case rsx::surface_color_format::w16z16y16x16:
			case rsx::surface_color_format::w32z32y32x32:
			case rsx::surface_color_format::x32:
				break;
			default:
				rop_control |= ROP_control::framebuffer_srgb_enable;
				break;
			}
		}

		// Generate wpos coefficients
		// wpos equation is now as follows:
		// wpos.y = (frag_coord / resolution_scale) * ((window_origin!=top)?-1.: 1.) + ((window_origin!=top)? window_height : 0)
		// wpos.x = (frag_coord / resolution_scale)
		// wpos.zw = frag_coord.zw

		const auto window_origin = rsx::method_registers.shader_window_origin();
		const u32 window_height = rsx::method_registers.shader_window_height();
		const f32 resolution_scale = (window_height <= static_cast<u32>(g_cfg.video.min_scalable_dimension)) ? 1.f : rsx::get_resolution_scale();
		const f32 wpos_scale = (window_origin == rsx::window_origin::top) ? (1.f / resolution_scale) : (-1.f / resolution_scale);
		const f32 wpos_bias = (window_origin == rsx::window_origin::top) ? 0.f : window_height;
		const f32 alpha_ref = rsx::method_registers.alpha_ref();

		u32 *dst = static_cast<u32*>(buffer);
		utils::stream_vector(dst, std::bit_cast<u32>(fog0), std::bit_cast<u32>(fog1), rop_control, std::bit_cast<u32>(alpha_ref));
		utils::stream_vector(dst + 4, 0u, fog_mode, std::bit_cast<u32>(wpos_scale), std::bit_cast<u32>(wpos_bias));
	}

	u64 thread::timestamp()
	{
		const u64 freq = sys_time_get_timebase_frequency();

		auto get_time_ns = [freq]()
		{
			const u64 t = get_timebased_time();
			return (t / freq * 1'000'000'000 + t % freq * 1'000'000'000 / freq);
		};

		const u64 t = get_time_ns();
		if (t != timestamp_ctrl)
		{
			timestamp_ctrl = t;
			timestamp_subvalue = 0;
			return t;
		}

		// Check if we passed the limit of what fixed increments is legal for
		// Wait for the next time value reported if we passed the limit
		if ((1'000'000'000 / freq) - timestamp_subvalue <= 2)
		{
			u64 now = get_time_ns();

			for (; t == now; now = get_time_ns())
			{
				utils::pause();
			}

			timestamp_ctrl = now;
			timestamp_subvalue = 0;
			return now;
		}

		timestamp_subvalue += 2;
		return t + timestamp_subvalue;
	}

	std::span<const std::byte> thread::get_raw_index_array(const draw_clause& draw_indexed_clause) const
	{
		if (!element_push_buffer.empty())
		{
			//Indices provided via immediate mode
			return{reinterpret_cast<const std::byte*>(element_push_buffer.data()), ::narrow<u32>(element_push_buffer.size() * sizeof(u32))};
		}

		const rsx::index_array_type type = rsx::method_registers.index_type();
		const u32 type_size = get_index_type_size(type);

		// Force aligned indices as realhw
		const u32 address = (0 - type_size) & get_address(rsx::method_registers.index_array_address(), rsx::method_registers.index_array_location());

		//const bool is_primitive_restart_enabled = rsx::method_registers.restart_index_enabled();
		//const u32 primitive_restart_index = rsx::method_registers.restart_index();

		const u32 first = draw_indexed_clause.min_index();
		const u32 count = draw_indexed_clause.get_elements_count();

		const auto ptr = vm::_ptr<const std::byte>(address);
		return{ ptr + first * type_size, count * type_size };
	}

	std::variant<draw_array_command, draw_indexed_array_command, draw_inlined_array>
	thread::get_draw_command(const rsx::rsx_state& state) const
	{
		if (rsx::method_registers.current_draw_clause.command == rsx::draw_command::array)
		{
			return draw_array_command{};
		}

		if (rsx::method_registers.current_draw_clause.command == rsx::draw_command::indexed)
		{
			return draw_indexed_array_command
			{
				get_raw_index_array(state.current_draw_clause)
			};
		}

		if (rsx::method_registers.current_draw_clause.command == rsx::draw_command::inlined_array)
		{
			return draw_inlined_array{};
		}

		fmt::throw_exception("ill-formed draw command");
	}

	void thread::do_local_task(FIFO_state state)
	{
		m_eng_interrupt_mask.clear(rsx::backend_interrupt);

		if (async_flip_requested & flip_request::emu_requested)
		{
			// NOTE: This has to be executed immediately
			// Delaying this operation can cause desync due to the delay in firing the flip event
			handle_emu_flip(async_flip_buffer);
		}

		if (!in_begin_end && state != FIFO_state::lock_wait)
		{
			if (atomic_storage<u32>::load(m_invalidated_memory_range.end) != 0)
			{
				std::lock_guard lock(m_mtx_task);

				if (m_invalidated_memory_range.valid())
				{
					handle_invalidated_memory_range();
				}
			}
		}

		if (m_eng_interrupt_mask & rsx::pipe_flush_interrupt)
		{
			sync();
		}
	}

	std::array<u32, 4> thread::get_color_surface_addresses() const
	{
		u32 offset_color[] =
		{
			rsx::method_registers.surface_offset(0),
			rsx::method_registers.surface_offset(1),
			rsx::method_registers.surface_offset(2),
			rsx::method_registers.surface_offset(3),
		};
		u32 context_dma_color[] =
		{
			rsx::method_registers.surface_dma(0),
			rsx::method_registers.surface_dma(1),
			rsx::method_registers.surface_dma(2),
			rsx::method_registers.surface_dma(3),
		};
		return
		{
			rsx::get_address(offset_color[0], context_dma_color[0]),
			rsx::get_address(offset_color[1], context_dma_color[1]),
			rsx::get_address(offset_color[2], context_dma_color[2]),
			rsx::get_address(offset_color[3], context_dma_color[3]),
		};
	}

	u32 thread::get_zeta_surface_address() const
	{
		u32 m_context_dma_z = rsx::method_registers.surface_z_dma();
		u32 offset_zeta = rsx::method_registers.surface_z_offset();
		return rsx::get_address(offset_zeta, m_context_dma_z);
	}

	void thread::get_framebuffer_layout(rsx::framebuffer_creation_context context, framebuffer_layout &layout)
	{
		layout = {};

		layout.ignore_change = true;
		layout.width = rsx::method_registers.surface_clip_width();
		layout.height = rsx::method_registers.surface_clip_height();

		framebuffer_status_valid = false;
		m_framebuffer_state_contested = false;
		m_current_framebuffer_context = context;

		if (layout.width == 0 || layout.height == 0)
		{
			rsx_log.trace("Invalid framebuffer setup, w=%d, h=%d", layout.width, layout.height);
			return;
		}

		//const u16 clip_x = rsx::method_registers.surface_clip_origin_x();
		//const u16 clip_y = rsx::method_registers.surface_clip_origin_y();

		layout.color_addresses = get_color_surface_addresses();
		layout.zeta_address = get_zeta_surface_address();
		layout.zeta_pitch = rsx::method_registers.surface_z_pitch();
		layout.color_pitch =
		{
			rsx::method_registers.surface_pitch(0),
			rsx::method_registers.surface_pitch(1),
			rsx::method_registers.surface_pitch(2),
			rsx::method_registers.surface_pitch(3),
		};

		layout.color_format = rsx::method_registers.surface_color();
		layout.depth_format = rsx::method_registers.surface_depth_fmt();
		layout.target = rsx::method_registers.surface_color_target();

		const auto mrt_buffers = rsx::utility::get_rtt_indexes(layout.target);
		const auto aa_mode = rsx::method_registers.surface_antialias();
		const u32 aa_factor_u = (aa_mode == rsx::surface_antialiasing::center_1_sample) ? 1 : 2;
		const u32 aa_factor_v = (aa_mode == rsx::surface_antialiasing::center_1_sample || aa_mode == rsx::surface_antialiasing::diagonal_centered_2_samples) ? 1 : 2;
		const u8 sample_count = get_format_sample_count(aa_mode);

		const auto depth_texel_size = get_format_block_size_in_bytes(layout.depth_format) * aa_factor_u;
		const auto color_texel_size = get_format_block_size_in_bytes(layout.color_format) * aa_factor_u;
		const bool stencil_test_enabled = is_depth_stencil_format(layout.depth_format) && rsx::method_registers.stencil_test_enabled();
		const bool depth_test_enabled = rsx::method_registers.depth_test_enabled();

		// Check write masks
		layout.zeta_write_enabled = (depth_test_enabled && rsx::method_registers.depth_write_enabled());
		if (!layout.zeta_write_enabled && stencil_test_enabled)
		{
			// Check if stencil data is modified
			auto mask = rsx::method_registers.stencil_mask();
			bool active_write_op = (rsx::method_registers.stencil_op_zpass() != rsx::stencil_op::keep ||
				rsx::method_registers.stencil_op_fail() != rsx::stencil_op::keep ||
				rsx::method_registers.stencil_op_zfail() != rsx::stencil_op::keep);

			if ((!mask || !active_write_op) && rsx::method_registers.two_sided_stencil_test_enabled())
			{
				mask |= rsx::method_registers.back_stencil_mask();
				active_write_op |= (rsx::method_registers.back_stencil_op_zpass() != rsx::stencil_op::keep ||
					rsx::method_registers.back_stencil_op_fail() != rsx::stencil_op::keep ||
					rsx::method_registers.back_stencil_op_zfail() != rsx::stencil_op::keep);
			}

			layout.zeta_write_enabled = (mask && active_write_op);
		}

		// NOTE: surface_target_a is index 1 but is not MRT since only one surface is active
		bool color_write_enabled = false;
		for (uint i = 0; i < mrt_buffers.size(); ++i)
		{
			if (rsx::method_registers.color_write_enabled(i))
			{
				const auto real_index = mrt_buffers[i];
				layout.color_write_enabled[real_index] = true;
				color_write_enabled = true;
			}
		}

		bool depth_buffer_unused = false, color_buffer_unused = false;

		switch (context)
		{
		case rsx::framebuffer_creation_context::context_clear_all:
			break;
		case rsx::framebuffer_creation_context::context_clear_depth:
			color_buffer_unused = true;
			break;
		case rsx::framebuffer_creation_context::context_clear_color:
			depth_buffer_unused = true;
			break;
		case rsx::framebuffer_creation_context::context_draw:
			// NOTE: As with all other hw, depth/stencil writes involve the corresponding depth/stencil test, i.e No test = No write
			// NOTE: Depth test is not really using the memory if its set to always or never
			// TODO: Perform similar checks for stencil test
			if (!stencil_test_enabled)
			{
				if (!depth_test_enabled)
				{
					depth_buffer_unused = true;
				}
				else if (!rsx::method_registers.depth_write_enabled())
				{
					// Depth test is enabled but depth write is disabled
					switch (rsx::method_registers.depth_func())
					{
					default:
						break;
					case rsx::comparison_function::never:
					case rsx::comparison_function::always:
						// No access to depth buffer memory
						depth_buffer_unused = true;
						break;
					}
				}

				if (depth_buffer_unused) [[unlikely]]
				{
					// Check if depth bounds is active. Depth bounds test does NOT need depth test to be enabled to access the Z buffer
					// Bind Z buffer in read mode for bounds check in this case
					if (rsx::method_registers.depth_bounds_test_enabled() &&
						(rsx::method_registers.depth_bounds_min() > 0.f || rsx::method_registers.depth_bounds_max() < 1.f))
					{
						depth_buffer_unused = false;
					}
				}
			}

			color_buffer_unused = !color_write_enabled || layout.target == rsx::surface_target::none;
			m_framebuffer_state_contested = color_buffer_unused || depth_buffer_unused;
			break;
		default:
			fmt::throw_exception("Unknown framebuffer context 0x%x", static_cast<u32>(context));
		}

		// Swizzled render does tight packing of bytes
		bool packed_render = false;
		u32 minimum_color_pitch = 64u;
		u32 minimum_zeta_pitch = 64u;

		switch (layout.raster_type = rsx::method_registers.surface_type())
		{
		default:
			rsx_log.error("Unknown raster mode 0x%x", static_cast<u32>(layout.raster_type));
			[[fallthrough]];
		case rsx::surface_raster_type::linear:
			break;
		case rsx::surface_raster_type::swizzle:
			packed_render = true;
			break;
		}

		if (!packed_render)
		{
			// Well, this is a write operation either way (clearing or drawing)
			// We can deduce a minimum pitch for which this operation is guaranteed to require by checking for the lesser of scissor or clip
			const u32 write_limit_x = std::min<u32>(layout.width, rsx::method_registers.scissor_origin_x() + rsx::method_registers.scissor_width());

			minimum_color_pitch = color_texel_size * write_limit_x;
			minimum_zeta_pitch = depth_texel_size * write_limit_x;
		}

		if (depth_buffer_unused)
		{
			layout.zeta_address = 0;
		}
		else if (layout.zeta_pitch < minimum_zeta_pitch)
		{
			layout.zeta_address = 0;
		}
		else if (packed_render)
		{
			layout.actual_zeta_pitch = (layout.width * depth_texel_size);
		}
		else
		{
			const auto packed_zeta_pitch = (layout.width * depth_texel_size);
			if (packed_zeta_pitch > layout.zeta_pitch)
			{
				layout.width = (layout.zeta_pitch / depth_texel_size);
			}

			layout.actual_zeta_pitch = layout.zeta_pitch;
		}

		for (const auto &index : rsx::utility::get_rtt_indexes(layout.target))
		{
			if (color_buffer_unused)
			{
				layout.color_addresses[index] = 0;
				continue;
			}

			if (layout.color_pitch[index] < minimum_color_pitch)
			{
				// Unlike the depth buffer, when given a color target we know it is intended to be rendered to
				rsx_log.error("Framebuffer setup error: Color target failed pitch check, Pitch=[%d, %d, %d, %d] + %d, target=%d, context=%d",
					layout.color_pitch[0], layout.color_pitch[1], layout.color_pitch[2], layout.color_pitch[3],
					layout.zeta_pitch, static_cast<u32>(layout.target), static_cast<u32>(context));

				// Do not remove this buffer for now as it implies something went horribly wrong anyway
				break;
			}

			if (layout.color_addresses[index] == layout.zeta_address)
			{
				rsx_log.warning("Framebuffer at 0x%X has aliasing color/depth targets, color_index=%d, zeta_pitch = %d, color_pitch=%d, context=%d",
					layout.zeta_address, index, layout.zeta_pitch, layout.color_pitch[index], static_cast<u32>(context));

				m_framebuffer_state_contested = true;

				// TODO: Research clearing both depth AND color
				// TODO: If context is creation_draw, deal with possibility of a lost buffer clear
				if (depth_test_enabled || stencil_test_enabled || (!layout.color_write_enabled[index] && layout.zeta_write_enabled))
				{
					// Use address for depth data
					layout.color_addresses[index] = 0;
					continue;
				}
				else
				{
					// Use address for color data
					layout.zeta_address = 0;
				}
			}

			ensure(layout.color_addresses[index]);

			const auto packed_pitch = (layout.width * color_texel_size);
			if (packed_render)
			{
				layout.actual_color_pitch[index] = packed_pitch;
			}
			else
			{
				if (packed_pitch > layout.color_pitch[index])
				{
					layout.width = (layout.color_pitch[index] / color_texel_size);
				}

				layout.actual_color_pitch[index] = layout.color_pitch[index];
			}

			framebuffer_status_valid = true;
		}

		if (!framebuffer_status_valid && !layout.zeta_address)
		{
			rsx_log.warning("Framebuffer setup failed. Draw calls may have been lost");
			return;
		}

		// At least one attachment exists
		framebuffer_status_valid = true;

		// Window (raster) offsets
		const auto window_offset_x = rsx::method_registers.window_offset_x();
		const auto window_offset_y = rsx::method_registers.window_offset_y();
		const auto window_clip_width = rsx::method_registers.window_clip_horizontal();
		const auto window_clip_height = rsx::method_registers.window_clip_vertical();

		if (window_offset_x || window_offset_y)
		{
			// Window offset is what affects the raster position!
			// Tested with Turbo: Super stunt squad that only changes the window offset to declare new framebuffers
			// Sampling behavior clearly indicates the addresses are expected to have changed
			if (auto clip_type = rsx::method_registers.window_clip_type())
				rsx_log.error("Unknown window clip type 0x%X", clip_type);

			for (const auto &index : rsx::utility::get_rtt_indexes(layout.target))
			{
				if (layout.color_addresses[index])
				{
					const u32 window_offset_bytes = (layout.actual_color_pitch[index] * window_offset_y) + (color_texel_size * window_offset_x);
					layout.color_addresses[index] += window_offset_bytes;
				}
			}

			if (layout.zeta_address)
			{
				layout.zeta_address += (layout.actual_zeta_pitch * window_offset_y) + (depth_texel_size * window_offset_x);
			}
		}

		if ((window_clip_width && window_clip_width < layout.width) ||
			(window_clip_height && window_clip_height < layout.height))
		{
			rsx_log.error("Unexpected window clip dimensions: window_clip=%dx%d, surface_clip=%dx%d",
				window_clip_width, window_clip_height, layout.width, layout.height);
		}

		layout.aa_mode = aa_mode;
		layout.aa_factors[0] = aa_factor_u;
		layout.aa_factors[1] = aa_factor_v;

		bool really_changed = false;

		for (u8 i = 0; i < rsx::limits::color_buffers_count; ++i)
		{
			if (m_surface_info[i].address != layout.color_addresses[i])
			{
				really_changed = true;
				break;
			}

			if (layout.color_addresses[i])
			{
				if (m_surface_info[i].width != layout.width ||
					m_surface_info[i].height != layout.height ||
					m_surface_info[i].color_format != layout.color_format ||
					m_surface_info[i].samples != sample_count)
				{
					really_changed = true;
					break;
				}
			}
		}

		if (!really_changed)
		{
			if (layout.zeta_address == m_depth_surface_info.address &&
				layout.depth_format == m_depth_surface_info.depth_format &&
				sample_count == m_depth_surface_info.samples)
			{
				// Same target is reused
				return;
			}
		}

		layout.ignore_change = false;
	}

	void thread::on_framebuffer_options_changed(u32 opt)
	{
		auto evaluate_depth_buffer_state = [&]()
		{
			m_framebuffer_layout.zeta_write_enabled =
				(rsx::method_registers.depth_test_enabled() && rsx::method_registers.depth_write_enabled());
		};

		auto evaluate_stencil_buffer_state = [&]()
		{
			if (!m_framebuffer_layout.zeta_write_enabled &&
				rsx::method_registers.stencil_test_enabled() &&
				is_depth_stencil_format(m_framebuffer_layout.depth_format))
			{
				// Check if stencil data is modified
				auto mask = rsx::method_registers.stencil_mask();
				bool active_write_op = (rsx::method_registers.stencil_op_zpass() != rsx::stencil_op::keep ||
					rsx::method_registers.stencil_op_fail() != rsx::stencil_op::keep ||
					rsx::method_registers.stencil_op_zfail() != rsx::stencil_op::keep);

				if ((!mask || !active_write_op) && rsx::method_registers.two_sided_stencil_test_enabled())
				{
					mask |= rsx::method_registers.back_stencil_mask();
					active_write_op |= (rsx::method_registers.back_stencil_op_zpass() != rsx::stencil_op::keep ||
						rsx::method_registers.back_stencil_op_fail() != rsx::stencil_op::keep ||
						rsx::method_registers.back_stencil_op_zfail() != rsx::stencil_op::keep);
				}

				m_framebuffer_layout.zeta_write_enabled = (mask && active_write_op);
			}
		};

		auto evaluate_color_buffer_state = [&]() -> bool
		{
			const auto mrt_buffers = rsx::utility::get_rtt_indexes(m_framebuffer_layout.target);
			bool any_found = false;

			for (uint i = 0; i < mrt_buffers.size(); ++i)
			{
				if (rsx::method_registers.color_write_enabled(i))
				{
					const auto real_index = mrt_buffers[i];
					m_framebuffer_layout.color_write_enabled[real_index] = true;
					any_found = true;
				}
			}

			return any_found;
		};

		auto evaluate_depth_buffer_contested = [&]()
		{
			if (m_framebuffer_layout.zeta_address) [[likely]]
			{
				// Nothing to do, depth buffer already exists
				return false;
			}

			// Check if depth read/write is enabled
			if (m_framebuffer_layout.zeta_write_enabled ||
				rsx::method_registers.depth_test_enabled())
			{
				return true;
			}

			// Check if stencil read is enabled
			if (is_depth_stencil_format(m_framebuffer_layout.depth_format) &&
				rsx::method_registers.stencil_test_enabled())
			{
				return true;
			}

			return false;
		};

		if (m_rtts_dirty)
		{
			// Nothing to do
			return;
		}

		switch (opt)
		{
		case NV4097_SET_DEPTH_TEST_ENABLE:
		case NV4097_SET_DEPTH_MASK:
		case NV4097_SET_DEPTH_FUNC:
		{
			evaluate_depth_buffer_state();

			if (m_framebuffer_state_contested)
			{
				m_rtts_dirty |= evaluate_depth_buffer_contested();
			}
			break;
		}
		case NV4097_SET_TWO_SIDED_STENCIL_TEST_ENABLE:
		case NV4097_SET_STENCIL_TEST_ENABLE:
		case NV4097_SET_STENCIL_MASK:
		case NV4097_SET_STENCIL_OP_ZPASS:
		case NV4097_SET_STENCIL_OP_FAIL:
		case NV4097_SET_STENCIL_OP_ZFAIL:
		case NV4097_SET_BACK_STENCIL_MASK:
		case NV4097_SET_BACK_STENCIL_OP_ZPASS:
		case NV4097_SET_BACK_STENCIL_OP_FAIL:
		case NV4097_SET_BACK_STENCIL_OP_ZFAIL:
		{
			// Stencil takes a back seat to depth buffer stuff
			evaluate_depth_buffer_state();

			if (!m_framebuffer_layout.zeta_write_enabled)
			{
				evaluate_stencil_buffer_state();
			}

			if (m_framebuffer_state_contested)
			{
				m_rtts_dirty |= evaluate_depth_buffer_contested();
			}
			break;
		}
		case NV4097_SET_COLOR_MASK:
		case NV4097_SET_COLOR_MASK_MRT:
		{
			if (!m_framebuffer_state_contested) [[likely]]
			{
				// Update write masks and continue
				evaluate_color_buffer_state();
			}
			else
			{
				bool old_state = false;
				for (const auto& enabled : m_framebuffer_layout.color_write_enabled)
				{
					if (old_state = enabled; old_state) break;
				}

				const auto new_state = evaluate_color_buffer_state();
				if (!old_state && new_state)
				{
					// Color buffers now in use
					m_rtts_dirty = true;
				}
			}
			break;
		}
		default:
			rsx_log.fatal("Unhandled framebuffer option changed 0x%x", opt);
		}
	}

	bool thread::get_scissor(areau& region, bool clip_viewport)
	{
		if (!(m_graphics_state & rsx::pipeline_state::scissor_config_state_dirty))
		{
			if (clip_viewport == !!(m_graphics_state & rsx::pipeline_state::scissor_setup_clipped))
			{
				// Nothing to do
				return false;
			}
		}

		m_graphics_state &= ~(rsx::pipeline_state::scissor_config_state_dirty | rsx::pipeline_state::scissor_setup_clipped);

		u16 x1, x2, y1, y2;

		u16 scissor_x = rsx::method_registers.scissor_origin_x();
		u16 scissor_w = rsx::method_registers.scissor_width();
		u16 scissor_y = rsx::method_registers.scissor_origin_y();
		u16 scissor_h = rsx::method_registers.scissor_height();

		if (clip_viewport)
		{
			u16 raster_x = rsx::method_registers.viewport_origin_x();
			u16 raster_w = rsx::method_registers.viewport_width();
			u16 raster_y = rsx::method_registers.viewport_origin_y();
			u16 raster_h = rsx::method_registers.viewport_height();

			// Get the minimum area between these two
			x1 = std::max(scissor_x, raster_x);
			y1 = std::max(scissor_y, raster_y);
			x2 = std::min(scissor_x + scissor_w, raster_x + raster_w);
			y2 = std::min(scissor_y + scissor_h, raster_y + raster_h);

			m_graphics_state |= rsx::pipeline_state::scissor_setup_clipped;
		}
		else
		{
			x1 = scissor_x;
			x2 = scissor_x + scissor_w;
			y1 = scissor_y;
			y2 = scissor_y + scissor_h;
		}

		if (x2 <= x1 ||
			y2 <= y1 ||
			x1 >= rsx::method_registers.window_clip_horizontal() ||
			y1 >= rsx::method_registers.window_clip_vertical())
		{
			m_graphics_state |= rsx::pipeline_state::scissor_setup_invalid;
			framebuffer_status_valid = false;
			return false;
		}

		if (m_graphics_state & rsx::pipeline_state::scissor_setup_invalid)
		{
			m_graphics_state &= ~rsx::pipeline_state::scissor_setup_invalid;
			framebuffer_status_valid = true;
		}

		std::tie(region.x1, region.y1) = rsx::apply_resolution_scale<false>(x1, y1, m_framebuffer_layout.width, m_framebuffer_layout.height);
		std::tie(region.x2, region.y2) = rsx::apply_resolution_scale<true>(x2, y2, m_framebuffer_layout.width, m_framebuffer_layout.height);

		return true;
	}

	void thread::prefetch_fragment_program()
	{
		if (!(m_graphics_state & rsx::pipeline_state::fragment_program_ucode_dirty))
			return;

		m_graphics_state &= ~rsx::pipeline_state::fragment_program_ucode_dirty;

		// Request for update of fragment constants if the program block is invalidated
		m_graphics_state |= rsx::pipeline_state::fragment_constants_dirty;

		const auto [program_offset, program_location] = method_registers.shader_program_address();
		const auto prev_textures_reference_mask = current_fp_metadata.referenced_textures_mask;

		auto data_ptr = vm::base(rsx::get_address(program_offset, program_location));
		current_fp_metadata = program_hash_util::fragment_program_utils::analyse_fragment_program(data_ptr);

		current_fragment_program.data = (static_cast<u8*>(data_ptr) + current_fp_metadata.program_start_offset);
		current_fragment_program.offset = program_offset + current_fp_metadata.program_start_offset;
		current_fragment_program.ucode_length = current_fp_metadata.program_ucode_length;
		current_fragment_program.total_length = current_fp_metadata.program_ucode_length + current_fp_metadata.program_start_offset;
		current_fragment_program.texture_state.import(current_fp_texture_state, current_fp_metadata.referenced_textures_mask);
		current_fragment_program.valid = true;

		if (!(m_graphics_state & rsx::pipeline_state::fragment_program_state_dirty))
		{
			// Verify current texture state is valid
			for (u32 textures_ref = current_fp_metadata.referenced_textures_mask, i = 0; textures_ref; textures_ref >>= 1, ++i)
			{
				if (!(textures_ref & 1)) continue;

				if (m_textures_dirty[i])
				{
					m_graphics_state |= rsx::pipeline_state::fragment_program_state_dirty;
					break;
				}
			}
		}

		if (!(m_graphics_state & rsx::pipeline_state::fragment_program_state_dirty) &&
			(prev_textures_reference_mask != current_fp_metadata.referenced_textures_mask))
		{
			// If different textures are used, upload their coefficients.
			// The texture parameters transfer routine is optimized and only writes data for textures consumed by the ucode.
			m_graphics_state |= rsx::pipeline_state::fragment_texture_state_dirty;
		}
	}

	void thread::prefetch_vertex_program()
	{
		if (!(m_graphics_state & rsx::pipeline_state::vertex_program_ucode_dirty))
			return;

		m_graphics_state &= ~rsx::pipeline_state::vertex_program_ucode_dirty;

		// Reload transform constants unconditionally for now
		m_graphics_state |= rsx::pipeline_state::transform_constants_dirty;

		const u32 transform_program_start = rsx::method_registers.transform_program_start();
		current_vertex_program.data.reserve(512 * 4);
		current_vertex_program.jump_table.clear();

		current_vp_metadata = program_hash_util::vertex_program_utils::analyse_vertex_program
		(
			method_registers.transform_program.data(),  // Input raw block
			transform_program_start,                    // Address of entry point
			current_vertex_program                      // [out] Program object
		);

		current_vertex_program.texture_state.import(current_vp_texture_state, current_vp_metadata.referenced_textures_mask);

		if (!(m_graphics_state & rsx::pipeline_state::vertex_program_state_dirty))
		{
			// Verify current texture state is valid
			for (u32 textures_ref = current_vp_metadata.referenced_textures_mask, i = 0; textures_ref; textures_ref >>= 1, ++i)
			{
				if (!(textures_ref & 1)) continue;

				if (m_vertex_textures_dirty[i])
				{
					m_graphics_state |= rsx::pipeline_state::vertex_program_state_dirty;
					break;
				}
			}
		}
	}

	void thread::analyse_current_rsx_pipeline()
	{
		prefetch_vertex_program();
		prefetch_fragment_program();
	}

	void thread::get_current_vertex_program(const std::array<std::unique_ptr<rsx::sampled_image_descriptor_base>, rsx::limits::vertex_textures_count>& sampler_descriptors)
	{
		if (!(m_graphics_state & rsx::pipeline_state::vertex_program_dirty))
			return;

		ensure(!(m_graphics_state & rsx::pipeline_state::vertex_program_ucode_dirty));
		current_vertex_program.output_mask = rsx::method_registers.vertex_attrib_output_mask();

		for (u32 textures_ref = current_vp_metadata.referenced_textures_mask, i = 0; textures_ref; textures_ref >>= 1, ++i)
		{
			if (!(textures_ref & 1)) continue;

			const auto &tex = rsx::method_registers.vertex_textures[i];
			if (tex.enabled() && (current_vp_metadata.referenced_textures_mask & (1 << i)))
			{
				current_vp_texture_state.clear(i);
				current_vp_texture_state.set_dimension(sampler_descriptors[i]->image_type, i);

				if (backend_config.supports_hw_msaa &&
					sampler_descriptors[i]->samples > 1)
				{
					current_vp_texture_state.multisampled_textures |= (1 << i);
				}
			}
		}

		current_vertex_program.texture_state.import(current_vp_texture_state, current_vp_metadata.referenced_textures_mask);
	}

	void thread::analyse_inputs_interleaved(vertex_input_layout& result)
	{
		const rsx_state& state = rsx::method_registers;
		const u32 input_mask = state.vertex_attrib_input_mask() & current_vp_metadata.referenced_inputs_mask;

		result.clear();

		if (state.current_draw_clause.command == rsx::draw_command::inlined_array)
		{
			interleaved_range_info info = {};
			info.interleaved = true;
			info.locations.reserve(8);

			for (u8 index = 0; index < rsx::limits::vertex_count; ++index)
			{
				auto &vinfo = state.vertex_arrays_info[index];

				if (vinfo.size() > 0)
				{
					// Stride must be updated even if the stream is disabled
					info.attribute_stride += rsx::get_vertex_type_size_on_host(vinfo.type(), vinfo.size());
					info.locations.push_back({ index, false, 1 });

					if (input_mask & (1u << index))
					{
						result.attribute_placement[index] = attribute_buffer_placement::transient;
					}
				}
				else if (state.register_vertex_info[index].size > 0 && input_mask & (1u << index))
				{
					//Reads from register
					result.referenced_registers.push_back(index);
					result.attribute_placement[index] = attribute_buffer_placement::transient;
				}
			}

			if (info.attribute_stride)
			{
				// At least one array feed must be enabled for vertex input
				result.interleaved_blocks.emplace_back(std::move(info));
			}

			return;
		}

		const u32 frequency_divider_mask = rsx::method_registers.frequency_divider_operation_mask();
		result.interleaved_blocks.reserve(16);
		result.referenced_registers.reserve(16);

		for (auto [ref_mask, index] = std::tuple{ input_mask, u8(0) }; ref_mask; ++index, ref_mask >>= 1)
		{
			ensure(index < rsx::limits::vertex_count);

			if (!(ref_mask & 1u))
			{
				// Nothing to do, uninitialized
				continue;
			}

			//Check for interleaving
			const auto &info = state.vertex_arrays_info[index];
			if (rsx::method_registers.current_draw_clause.is_immediate_draw &&
				rsx::method_registers.current_draw_clause.command != rsx::draw_command::indexed)
			{
				// NOTE: In immediate rendering mode, all vertex setup is ignored
				// Observed with GT5, immediate render bypasses array pointers completely, even falling back to fixed-function register defaults
				if (vertex_push_buffers[index].vertex_count > 1)
				{
					// Ensure consistent number of vertices per attribute.
					vertex_push_buffers[index].pad_to(vertex_push_buffers[0].vertex_count, false);

					// Read temp buffer (register array)
					std::pair<u8, u32> volatile_range_info = std::make_pair(index, static_cast<u32>(vertex_push_buffers[index].data.size() * sizeof(u32)));
					result.volatile_blocks.push_back(volatile_range_info);
					result.attribute_placement[index] = attribute_buffer_placement::transient;
				}
				else if (state.register_vertex_info[index].size > 0)
				{
					// Reads from register
					result.referenced_registers.push_back(index);
					result.attribute_placement[index] = attribute_buffer_placement::transient;
				}

				// Fall back to the default register value if no source is specified via register
				continue;
			}

			if (!info.size())
			{
				if (state.register_vertex_info[index].size > 0)
				{
					//Reads from register
					result.referenced_registers.push_back(index);
					result.attribute_placement[index] = attribute_buffer_placement::transient;
					continue;
				}
			}
			else
			{
				result.attribute_placement[index] = attribute_buffer_placement::persistent;
				const u32 base_address = info.offset() & 0x7fffffff;
				bool alloc_new_block = true;
				bool modulo = !!(frequency_divider_mask & (1 << index));

				for (auto &block : result.interleaved_blocks)
				{
					if (block.single_vertex)
					{
						//Single vertex definition, continue
						continue;
					}

					if (block.attribute_stride != info.stride())
					{
						//Stride does not match, continue
						continue;
					}

					if (base_address > block.base_offset)
					{
						const u32 diff = base_address - block.base_offset;
						if (diff > info.stride())
						{
							//Not interleaved, continue
							continue;
						}
					}
					else
					{
						const u32 diff = block.base_offset - base_address;
						if (diff > info.stride())
						{
							//Not interleaved, continue
							continue;
						}

						//Matches, and this address is lower than existing
						block.base_offset = base_address;
					}

					alloc_new_block = false;
					block.locations.push_back({ index, modulo, info.frequency() });
					block.interleaved = true;
					break;
				}

				if (alloc_new_block)
				{
					interleaved_range_info block = {};
					block.base_offset = base_address;
					block.attribute_stride = info.stride();
					block.memory_location = info.offset() >> 31;
					block.locations.reserve(16);
					block.locations.push_back({ index, modulo, info.frequency() });

					if (block.attribute_stride == 0)
					{
						block.single_vertex = true;
						block.attribute_stride = rsx::get_vertex_type_size_on_host(info.type(), info.size());
					}

					result.interleaved_blocks.emplace_back(std::move(block));
				}
			}
		}

		for (auto &info : result.interleaved_blocks)
		{
			//Calculate real data address to be used during upload
			info.real_offset_address = rsx::get_address(rsx::get_vertex_offset_from_base(state.vertex_data_base_offset(), info.base_offset), info.memory_location);
		}
	}

	void thread::get_current_fragment_program(const std::array<std::unique_ptr<rsx::sampled_image_descriptor_base>, rsx::limits::fragment_textures_count>& sampler_descriptors)
	{
		if (!(m_graphics_state & rsx::pipeline_state::fragment_program_dirty))
			return;

		ensure(!(m_graphics_state & rsx::pipeline_state::fragment_program_ucode_dirty));

		m_graphics_state &= ~(rsx::pipeline_state::fragment_program_dirty);

		current_fragment_program.ctrl = rsx::method_registers.shader_control() & (CELL_GCM_SHADER_CONTROL_32_BITS_EXPORTS | CELL_GCM_SHADER_CONTROL_DEPTH_EXPORT);
		current_fragment_program.texcoord_control_mask = rsx::method_registers.texcoord_control_mask();
		current_fragment_program.two_sided_lighting = rsx::method_registers.two_side_light_en();

		if (method_registers.current_draw_clause.primitive == primitive_type::points &&
			method_registers.point_sprite_enabled())
		{
			// Set high word of the control mask to store point sprite control
			current_fragment_program.texcoord_control_mask |= u32(method_registers.point_sprite_control_mask()) << 16;
		}

		for (u32 textures_ref = current_fp_metadata.referenced_textures_mask, i = 0; textures_ref; textures_ref >>= 1, ++i)
		{
			if (!(textures_ref & 1)) continue;

			auto &tex = rsx::method_registers.fragment_textures[i];
			current_fp_texture_state.clear(i);

			if (tex.enabled() && sampler_descriptors[i]->format_class != RSX_FORMAT_CLASS_UNDEFINED)
			{
				current_fragment_program.texture_params[i].scale[0] = sampler_descriptors[i]->scale_x;
				current_fragment_program.texture_params[i].scale[1] = sampler_descriptors[i]->scale_y;
				current_fragment_program.texture_params[i].scale[2] = sampler_descriptors[i]->scale_z;
				current_fragment_program.texture_params[i].subpixel_bias = 0.f;
				current_fragment_program.texture_params[i].remap = tex.remap();

				m_graphics_state |= rsx::pipeline_state::fragment_texture_state_dirty;

				u32 texture_control = 0;
				current_fp_texture_state.set_dimension(sampler_descriptors[i]->image_type, i);

				if (tex.alpha_kill_enabled())
				{
					//alphakill can be ignored unless a valid comparison function is set
					texture_control |= (1 << texture_control_bits::ALPHAKILL);
				}

				//const u32 texaddr = rsx::get_address(tex.offset(), tex.location());
				const u32 raw_format = tex.format();
				const u32 format = raw_format & ~(CELL_GCM_TEXTURE_LN | CELL_GCM_TEXTURE_UN);

				if (raw_format & CELL_GCM_TEXTURE_UN)
				{
					if (tex.min_filter() == rsx::texture_minify_filter::nearest ||
						tex.mag_filter() == rsx::texture_magnify_filter::nearest)
					{
						// Subpixel offset so that (X + bias) * scale will round correctly.
						// This is done to work around fdiv precision issues in some GPUs (NVIDIA)
						current_fragment_program.texture_params[i].subpixel_bias = 0.01f;
					}
				}

				if (backend_config.supports_hw_msaa &&
					sampler_descriptors[i]->samples > 1)
				{
					current_fp_texture_state.multisampled_textures |= (1 << i);
					texture_control |= (static_cast<u32>(tex.zfunc()) << texture_control_bits::DEPTH_COMPARE_OP);
					texture_control |= (static_cast<u32>(tex.mag_filter() != rsx::texture_magnify_filter::nearest) << texture_control_bits::FILTERED_MAG);
					texture_control |= (static_cast<u32>(tex.min_filter() != rsx::texture_minify_filter::nearest) << texture_control_bits::FILTERED_MIN);
					texture_control |= (((tex.format() & CELL_GCM_TEXTURE_UN) >> 6) << texture_control_bits::UNNORMALIZED_COORDS);
				}

				if (sampler_descriptors[i]->format_class != RSX_FORMAT_CLASS_COLOR)
				{
					switch (sampler_descriptors[i]->format_class)
					{
					case RSX_FORMAT_CLASS_DEPTH16_FLOAT:
					case RSX_FORMAT_CLASS_DEPTH24_FLOAT_X8_PACK32:
						texture_control |= (1 << texture_control_bits::DEPTH_FLOAT);
						break;
					default:
						break;
					}

					switch (format)
					{
					case CELL_GCM_TEXTURE_A8R8G8B8:
					case CELL_GCM_TEXTURE_D8R8G8B8:
					{
						// Emulate bitcast in shader
						current_fp_texture_state.redirected_textures |= (1 << i);
						const auto float_en = (sampler_descriptors[i]->format_class == RSX_FORMAT_CLASS_DEPTH24_FLOAT_X8_PACK32)? 1 : 0;
						texture_control |= (float_en << texture_control_bits::DEPTH_FLOAT);
						break;
					}
					case CELL_GCM_TEXTURE_X16:
					{
						// A simple way to quickly read DEPTH16 data without shadow comparison
						break;
					}
					case CELL_GCM_TEXTURE_DEPTH16:
					case CELL_GCM_TEXTURE_DEPTH24_D8:
					case CELL_GCM_TEXTURE_DEPTH16_FLOAT:
					case CELL_GCM_TEXTURE_DEPTH24_D8_FLOAT:
					{
						// Natively supported Z formats with shadow comparison feature
						const auto compare_mode = tex.zfunc();
						if (!tex.alpha_kill_enabled() &&
							compare_mode < rsx::comparison_function::always &&
							compare_mode > rsx::comparison_function::never)
						{
							current_fp_texture_state.shadow_textures |= (1 << i);
						}
						break;
					}
					default:
						rsx_log.error("Depth texture bound to pipeline with unexpected format 0x%X", format);
					}
				}
				else if (!backend_config.supports_hw_renormalization)
				{
					switch (format)
					{
					case CELL_GCM_TEXTURE_A1R5G5B5:
					case CELL_GCM_TEXTURE_A4R4G4B4:
					case CELL_GCM_TEXTURE_D1R5G5B5:
					case CELL_GCM_TEXTURE_R5G5B5A1:
					case CELL_GCM_TEXTURE_R5G6B5:
					case CELL_GCM_TEXTURE_R6G5B5:
						texture_control |= (1 << texture_control_bits::RENORMALIZE);
						break;
					default:
						break;
					}
				}

				// Special operations applied to 8-bit formats such as gamma correction and sign conversion
				// NOTE: The unsigned_remap being set to anything other than 0 flags the texture as being signed (UE3)
				// This is a separate method of setting the format to signed mode without doing so per-channel
				// Precedence = SIGNED override > GAMMA > UNSIGNED_REMAP (See Resistance 3 for GAMMA/REMAP relationship, UE3 for REMAP effect)

				const u32 argb8_signed = tex.argb_signed();
				const u32 gamma = tex.gamma() & ~argb8_signed;
				const u32 unsigned_remap = (tex.unsigned_remap() == CELL_GCM_TEXTURE_UNSIGNED_REMAP_NORMAL)? 0u : (~gamma & 0xF);
				u32 argb8_convert = gamma;

				if (const u32 sign_convert = (argb8_signed | unsigned_remap))
				{
					// Apply remap to avoid mapping 1 to -1. Only the sign conversion needs this check
					// TODO: Use actual remap mask to account for 0 and 1 overrides in default mapping
					// TODO: Replace this clusterfuck of texture control with matrix transformation
					const auto remap_ctrl = (tex.remap() >> 8) & 0xAA;
					if (remap_ctrl == 0xAA)
					{
						argb8_convert |= (sign_convert & 0xFu) << texture_control_bits::EXPAND_OFFSET;
					}
					else
					{
						if (remap_ctrl & 0x03) argb8_convert |= (sign_convert & 0x1u) << texture_control_bits::EXPAND_OFFSET;
						if (remap_ctrl & 0x0C) argb8_convert |= (sign_convert & 0x2u) << texture_control_bits::EXPAND_OFFSET;
						if (remap_ctrl & 0x30) argb8_convert |= (sign_convert & 0x4u) << texture_control_bits::EXPAND_OFFSET;
						if (remap_ctrl & 0xC0) argb8_convert |= (sign_convert & 0x8u) << texture_control_bits::EXPAND_OFFSET;
					}
				}

				if (argb8_convert)
				{
					switch (format)
					{
					case CELL_GCM_TEXTURE_DEPTH24_D8:
					case CELL_GCM_TEXTURE_DEPTH24_D8_FLOAT:
					case CELL_GCM_TEXTURE_DEPTH16:
					case CELL_GCM_TEXTURE_DEPTH16_FLOAT:
					case CELL_GCM_TEXTURE_X16:
					case CELL_GCM_TEXTURE_Y16_X16:
					case CELL_GCM_TEXTURE_COMPRESSED_HILO8:
					case CELL_GCM_TEXTURE_COMPRESSED_HILO_S8:
					case CELL_GCM_TEXTURE_W16_Z16_Y16_X16_FLOAT:
					case CELL_GCM_TEXTURE_W32_Z32_Y32_X32_FLOAT:
					case CELL_GCM_TEXTURE_X32_FLOAT:
					case CELL_GCM_TEXTURE_Y16_X16_FLOAT:
						// Special data formats (XY, HILO, DEPTH) are not RGB formats
						// Ignore gamma flags
						break;
					default:
						texture_control |= argb8_convert;
						break;
					}
				}

				current_fragment_program.texture_params[i].control = texture_control;
			}
		}

		// Update texture configuration
		current_fragment_program.texture_state.import(current_fp_texture_state, current_fp_metadata.referenced_textures_mask);

		//Sanity checks
		if (current_fragment_program.ctrl & CELL_GCM_SHADER_CONTROL_DEPTH_EXPORT)
		{
			//Check that the depth stage is not disabled
			if (!rsx::method_registers.depth_test_enabled())
			{
				rsx_log.error("FS exports depth component but depth test is disabled (INVALID_OPERATION)");
			}
		}
	}

	bool thread::invalidate_fragment_program(u32 dst_dma, u32 dst_offset, u32 size)
	{
		const auto [shader_offset, shader_dma] = rsx::method_registers.shader_program_address();

		if ((dst_dma & CELL_GCM_LOCATION_MAIN) == shader_dma &&
		address_range::start_length(shader_offset, current_fragment_program.total_length).overlaps(
			address_range::start_length(dst_offset, size))) [[unlikely]]
		{
			// Data overlaps
			m_graphics_state |= rsx::pipeline_state::fragment_program_ucode_dirty;
			return true;
		}

		return false;
	}

	void thread::reset()
	{
		rsx::method_registers.reset();
	}

	void thread::init(u32 ctrlAddress)
	{
		dma_address = ctrlAddress;
		ctrl = vm::_ptr<RsxDmaControl>(ctrlAddress);
		flip_status = CELL_GCM_DISPLAY_FLIP_STATUS_DONE;

		std::memset(display_buffers, 0, sizeof(display_buffers));

		m_rsx_thread_exiting = false;
	}

	std::pair<u32, u32> thread::calculate_memory_requirements(const vertex_input_layout& layout, u32 first_vertex, u32 vertex_count)
	{
		u32 persistent_memory_size = 0;
		u32 volatile_memory_size = 0;

		volatile_memory_size += ::size32(layout.referenced_registers) * 16u;

		if (rsx::method_registers.current_draw_clause.command == rsx::draw_command::inlined_array)
		{
			for (const auto &block : layout.interleaved_blocks)
			{
				volatile_memory_size += block.attribute_stride * vertex_count;
			}
		}
		else
		{
			//NOTE: Immediate commands can be index array only or both index array and vertex data
			//Check both - but only check volatile blocks if immediate_draw flag is set
			if (rsx::method_registers.current_draw_clause.is_immediate_draw)
			{
				for (const auto &info : layout.volatile_blocks)
				{
					volatile_memory_size += info.second;
				}
			}

			persistent_memory_size = layout.calculate_interleaved_memory_requirements(first_vertex, vertex_count);
		}

		return std::make_pair(persistent_memory_size, volatile_memory_size);
	}

	void thread::fill_vertex_layout_state(const vertex_input_layout& layout, u32 first_vertex, u32 vertex_count, s32* buffer, u32 persistent_offset_base, u32 volatile_offset_base)
	{
		std::array<s32, 16> offset_in_block = {};
		u32 volatile_offset = volatile_offset_base;
		u32 persistent_offset = persistent_offset_base;

		//NOTE: Order is important! Transient ayout is always push_buffers followed by register data
		if (rsx::method_registers.current_draw_clause.is_immediate_draw)
		{
			for (const auto &info : layout.volatile_blocks)
			{
				offset_in_block[info.first] = volatile_offset;
				volatile_offset += info.second;
			}
		}

		for (u8 index : layout.referenced_registers)
		{
			offset_in_block[index] = volatile_offset;
			volatile_offset += 16;
		}

		if (rsx::method_registers.current_draw_clause.command == rsx::draw_command::inlined_array)
		{
			const auto &block = layout.interleaved_blocks[0];
			u32 inline_data_offset = volatile_offset;
			for (const auto& attrib : block.locations)
			{
				auto &info = rsx::method_registers.vertex_arrays_info[attrib.index];

				offset_in_block[attrib.index] = inline_data_offset;
				inline_data_offset += rsx::get_vertex_type_size_on_host(info.type(), info.size());
			}
		}
		else
		{
			for (const auto &block : layout.interleaved_blocks)
			{
				for (const auto& attrib : block.locations)
				{
					const u32 local_address = (rsx::method_registers.vertex_arrays_info[attrib.index].offset() & 0x7fffffff);
					offset_in_block[attrib.index] = persistent_offset + (local_address - block.base_offset);
				}

				const auto range = block.calculate_required_range(first_vertex, vertex_count);
				persistent_offset += block.attribute_stride * range.second;
			}
		}

		// Fill the data
		// Each descriptor field is 64 bits wide
		// [0-8] attribute stride
		// [8-24] attribute divisor
		// [24-27] attribute type
		// [27-30] attribute size
		// [30-31] reserved
		// [31-60] starting offset
		// [60-21] swap bytes flag
		// [61-22] volatile flag
		// [62-63] modulo enable flag

		const s32 default_frequency_mask = (1 << 8);
		const s32 swap_storage_mask = (1 << 29);
		const s32 volatile_storage_mask = (1 << 30);
		const s32 modulo_op_frequency_mask = smin;

		const u32 modulo_mask = rsx::method_registers.frequency_divider_operation_mask();
		const auto max_index = (first_vertex + vertex_count) - 1;

		for (u16 ref_mask = current_vp_metadata.referenced_inputs_mask, index = 0; ref_mask; ++index, ref_mask >>= 1)
		{
			if (!(ref_mask & 1u))
			{
				// Unused input, ignore this
				continue;
			}

			if (layout.attribute_placement[index] == attribute_buffer_placement::none)
			{
				static constexpr u64 zero = 0;
				std::memcpy(buffer + index * 2, &zero, sizeof(zero));
				continue;
			}

			rsx::vertex_base_type type = {};
			s32 size = 0;
			s32 attrib0 = 0;
			s32 attrib1 = 0;

			if (layout.attribute_placement[index] == attribute_buffer_placement::transient)
			{
				if (rsx::method_registers.current_draw_clause.command == rsx::draw_command::inlined_array)
				{
					const auto &info = rsx::method_registers.vertex_arrays_info[index];

					if (!info.size())
					{
						// Register
						const auto& reginfo = rsx::method_registers.register_vertex_info[index];
						type = reginfo.type;
						size = reginfo.size;

						attrib0 = rsx::get_vertex_type_size_on_host(type, size);
					}
					else
					{
						// Array
						type = info.type();
						size = info.size();

						attrib0 = layout.interleaved_blocks[0].attribute_stride | default_frequency_mask;
					}
				}
				else
				{
					// Data is either from an immediate render or register input
					// Immediate data overrides register input

					if (rsx::method_registers.current_draw_clause.is_immediate_draw &&
						vertex_push_buffers[index].vertex_count > 1)
					{
						// Push buffer
						const auto &info = vertex_push_buffers[index];
						type = info.type;
						size = info.size;

						attrib0 = rsx::get_vertex_type_size_on_host(type, size) | default_frequency_mask;
					}
					else
					{
						// Register
						const auto& info = rsx::method_registers.register_vertex_info[index];
						type = info.type;
						size = info.size;

						attrib0 = rsx::get_vertex_type_size_on_host(type, size);
					}
				}

				attrib1 |= volatile_storage_mask;
			}
			else
			{
				auto &info = rsx::method_registers.vertex_arrays_info[index];
				type = info.type();
				size = info.size();

				auto stride = info.stride();
				attrib0 = stride;

				if (stride > 0) //when stride is 0, input is not an array but a single element
				{
					const u32 frequency = info.frequency();
					switch (frequency)
					{
					case 0:
					case 1:
					{
						attrib0 |= default_frequency_mask;
						break;
					}
					default:
					{
						if (modulo_mask & (1 << index))
						{
							if (max_index >= frequency)
							{
								// Only set modulo mask if a modulo op is actually necessary!
								// This requires that the uploaded range for this attr = [0, freq-1]
								// Ignoring modulo op if the rendered range does not wrap allows for range optimization
								attrib0 |= (frequency << 8);
								attrib1 |= modulo_op_frequency_mask;
							}
							else
							{
								attrib0 |= default_frequency_mask;
							}
						}
						else
						{
							// Division
							attrib0 |= (frequency << 8);
						}
						break;
					}
					}
				}
			} //end attribute placement check

			// Special compressed 4 components into one 4-byte value. Decoded as one value.
			if (type == rsx::vertex_base_type::cmp)
			{
				size = 1;
			}

			// All data is passed in in PS3-native order (BE) so swap flag should be set
			attrib1 |= swap_storage_mask;
			attrib0 |= (static_cast<s32>(type) << 24);
			attrib0 |= (size << 27);
			attrib1 |= offset_in_block[index];

			buffer[index * 2 + 0] = attrib0;
			buffer[index * 2 + 1] = attrib1;
		}
	}

	void thread::write_vertex_data_to_memory(const vertex_input_layout& layout, u32 first_vertex, u32 vertex_count, void *persistent_data, void *volatile_data)
	{
		auto transient = static_cast<char*>(volatile_data);
		auto persistent = static_cast<char*>(persistent_data);

		auto &draw_call = rsx::method_registers.current_draw_clause;

		if (transient != nullptr)
		{
			if (draw_call.command == rsx::draw_command::inlined_array)
			{
				for (const u8 index : layout.referenced_registers)
				{
					memcpy(transient, rsx::method_registers.register_vertex_info[index].data.data(), 16);
					transient += 16;
				}

				memcpy(transient, draw_call.inline_vertex_array.data(), draw_call.inline_vertex_array.size() * sizeof(u32));
				//Is it possible to reference data outside of the inlined array?
				return;
			}

			//NOTE: Order is important! Transient layout is always push_buffers followed by register data
			if (draw_call.is_immediate_draw)
			{
				//NOTE: It is possible for immediate draw to only contain index data, so vertex data can be in persistent memory
				for (const auto &info : layout.volatile_blocks)
				{
					memcpy(transient, vertex_push_buffers[info.first].data.data(), info.second);
					transient += info.second;
				}
			}

			for (const u8 index : layout.referenced_registers)
			{
				memcpy(transient, rsx::method_registers.register_vertex_info[index].data.data(), 16);
				transient += 16;
			}
		}

		if (persistent != nullptr)
		{
			for (const auto &block : layout.interleaved_blocks)
			{
				auto range = block.calculate_required_range(first_vertex, vertex_count);

				const u32 data_size = range.second * block.attribute_stride;
				const u32 vertex_base = range.first * block.attribute_stride;

				g_fxo->get<rsx::dma_manager>().copy(persistent, vm::_ptr<char>(block.real_offset_address) + vertex_base, data_size);
				persistent += data_size;
			}
		}
	}

	void thread::flip(const display_flip_info_t& info)
	{
		m_eng_interrupt_mask.clear(rsx::display_interrupt);

		if (async_flip_requested & flip_request::any)
		{
			// Deferred flip
			if (info.emu_flip)
			{
				async_flip_requested.clear(flip_request::emu_requested);
			}
			else
			{
				async_flip_requested.clear(flip_request::native_ui);
			}
		}

		if (info.emu_flip)
		{
			performance_counters.sampled_frames++;
		}

		last_host_flip_timestamp = rsx::uclock();
	}

	void thread::check_zcull_status(bool framebuffer_swap)
	{
		if (framebuffer_swap)
		{
			zcull_surface_active = false;
			const u32 zeta_address = m_depth_surface_info.address;

			if (zeta_address)
			{
				//Find zeta address in bound zculls
				for (const auto& zcull : zculls)
				{
					if (zcull.bound &&
						rsx::to_surface_depth_format(zcull.zFormat) == m_depth_surface_info.depth_format &&
						rsx::to_surface_antialiasing(zcull.aaFormat) == rsx::method_registers.surface_antialias())
					{
						const u32 rsx_address = rsx::get_address(zcull.offset, CELL_GCM_LOCATION_LOCAL);
						if (rsx_address == zeta_address)
						{
							zcull_surface_active = true;
							break;
						}
					}
				}
			}
		}

		zcull_ctrl->set_enabled(this, zcull_rendering_enabled);
		zcull_ctrl->set_status(this, zcull_surface_active, zcull_pixel_cnt_enabled, zcull_stats_enabled);
	}

	void thread::clear_zcull_stats(u32 type)
	{
		zcull_ctrl->clear(this, type);
	}

	void thread::get_zcull_stats(u32 type, vm::addr_t sink)
	{
		u32 value = 0;
		if (!g_cfg.video.disable_zcull_queries)
		{
			switch (type)
			{
			case CELL_GCM_ZPASS_PIXEL_CNT:
			case CELL_GCM_ZCULL_STATS:
			case CELL_GCM_ZCULL_STATS1:
			case CELL_GCM_ZCULL_STATS2:
			case CELL_GCM_ZCULL_STATS3:
			{
				zcull_ctrl->read_report(this, sink, type);
				return;
			}
			default:
				rsx_log.error("Unknown zcull stat type %d", type);
				break;
			}
		}
		else if (g_cfg.video.zcull_hack)
		{
			value = g_cfg.video.zcull_default_value;
		}
		rsx::reservation_lock<true> lock(sink, 16);
		vm::_ref<atomic_t<CellGcmReportData>>(sink).store({ timestamp(), value, 0});
	}

	u32 thread::copy_zcull_stats(u32 memory_range_start, u32 memory_range, u32 destination)
	{
		return zcull_ctrl->copy_reports_to(memory_range_start, memory_range, destination);
	}

	void thread::enable_conditional_rendering(vm::addr_t ref)
	{
		cond_render_ctrl.enable_conditional_render(this, ref);

		auto result = zcull_ctrl->find_query(ref, true);
		if (result.found)
		{
			if (!result.queries.empty())
			{
				cond_render_ctrl.set_eval_sources(result.queries);
				sync_hint(FIFO_hint::hint_conditional_render_eval, { .query = cond_render_ctrl.eval_sources.front(), .address = ref });
			}
			else
			{
				bool failed = (result.raw_zpass_result == 0);
				cond_render_ctrl.set_eval_result(this, failed);
			}
		}
		else
		{
			cond_render_ctrl.eval_result(this);
		}
	}

	void thread::disable_conditional_rendering()
	{
		cond_render_ctrl.disable_conditional_render(this);
	}

	void thread::begin_conditional_rendering(const std::vector<reports::occlusion_query_info*>& /*sources*/)
	{
		cond_render_ctrl.hw_cond_active = true;
		cond_render_ctrl.eval_sources.clear();
	}

	void thread::end_conditional_rendering()
	{
		cond_render_ctrl.hw_cond_active = false;
	}

	void thread::sync()
	{
		m_eng_interrupt_mask.clear(rsx::pipe_flush_interrupt);

		if (zcull_ctrl->has_pending())
		{
			zcull_ctrl->sync(this);
		}

		// Fragment constants may have been updated
		m_graphics_state |= rsx::pipeline_state::fragment_constants_dirty;

		// DMA sync; if you need this, don't use MTRSX
		// g_fxo->get<rsx::dma_manager>().sync();

		//TODO: On sync every sub-unit should finish any pending tasks
		//Might cause zcull lockup due to zombie 'unclaimed reports' which are not forcefully removed currently
		//ensure(async_tasks_pending.load() == 0);
	}

	void thread::sync_hint(FIFO_hint /*hint*/, rsx::reports::sync_hint_payload_t payload)
	{
		zcull_ctrl->on_sync_hint(payload);
	}

	bool thread::is_fifo_idle() const
	{
		return ctrl == nullptr || ctrl->get == (ctrl->put & ~3);
	}

	void thread::flush_fifo()
	{
		// Make sure GET value is exposed before sync points
		fifo_ctrl->sync_get();
	}

	std::pair<u32, u32> thread::try_get_pc_of_x_cmds_backwards(u32 count, u32 get) const
	{
		if (!ctrl)
		{
			return {0, umax};
		}

		if (!count)
		{
			return {0, get};
		}

		u32 true_get = ctrl->get;
		u32 start = last_known_code_start;

		RSXDisAsm disasm(cpu_disasm_mode::survey_cmd_size, vm::g_sudo_addr, 0, this);

		std::vector<u32> pcs_of_valid_cmds;
		pcs_of_valid_cmds.reserve(std::min<u32>((get - start) / 16, 0x4000)); // Rough estimation of final array size

		auto probe_code_region = [&](u32 probe_start) -> std::pair<u32, u32>
		{
			pcs_of_valid_cmds.clear();
			pcs_of_valid_cmds.push_back(probe_start);

			while (pcs_of_valid_cmds.back() < get)
			{
				if (u32 advance = disasm.disasm(pcs_of_valid_cmds.back()))
				{
					pcs_of_valid_cmds.push_back(pcs_of_valid_cmds.back() + advance);
				}
				else
				{
					return {0, get};
				}
			}

			if (pcs_of_valid_cmds.size() == 1u || pcs_of_valid_cmds.back() != get)
			{
				return {0, get};
			}

			u32 found_cmds_count = std::min(count, ::size32(pcs_of_valid_cmds) - 1);

			return {found_cmds_count, *(pcs_of_valid_cmds.end() - 1 - found_cmds_count)};
		};

		auto pair = probe_code_region(start);

		if (!pair.first)
		{
			pair = probe_code_region(true_get);
		}

		return pair;
	}

	void thread::recover_fifo(u32 line, u32 col, const char* file, const char* func)
	{
		const u64 current_time = rsx::uclock();

		if (recovered_fifo_cmds_history.size() == 20u)
		{
			const auto cmd_info = recovered_fifo_cmds_history.front();

			// Check timestamp of last tracked cmd
			// Shorten the range of forbidden difference if driver wake-up delay is used
			if (current_time - cmd_info.timestamp < 2'000'000u - std::min<u32>(g_cfg.video.driver_wakeup_delay * 700, 1'400'000))
			{
				// Probably hopeless
				fmt::throw_exception("Dead FIFO commands queue state has been detected!\nTry increasing \"Driver Wake-Up Delay\" setting in Advanced settings. Called from %s", src_loc{line, col, file, func});
			}

			// Erase the last command from history, keep the size of the queue the same
			recovered_fifo_cmds_history.pop();
		}

		// Error. Should reset the queue
		fifo_ctrl->set_get(restore_point);
		fifo_ret_addr = saved_fifo_ret;
		std::this_thread::sleep_for(2ms);
		fifo_ctrl->abort();

		if (std::exchange(in_begin_end, false) && !rsx::method_registers.current_draw_clause.empty())
		{
			execute_nop_draw();
			rsx::thread::end();
		}

		recovered_fifo_cmds_history.push({fifo_ctrl->last_cmd(), current_time});
	}

	std::vector<std::pair<u32, u32>> thread::dump_callstack_list() const
	{
		std::vector<std::pair<u32, u32>> result;

		if (u32 addr = fifo_ret_addr; addr != RSX_CALL_STACK_EMPTY)
		{
			result.emplace_back(addr, 0);
		}

		return result;
	}

	void thread::fifo_wake_delay(u64 div)
	{
		// TODO: Nanoseconds accuracy
		u64 remaining = g_cfg.video.driver_wakeup_delay;

		if (!remaining)
		{
			return;
		}

		// Some cases do not need full delay
		remaining = utils::aligned_div(remaining, div);
		const u64 until = rsx::uclock() + remaining;

		while (true)
		{
#ifdef __linux__
			// NOTE: Assumption that timer initialization has succeeded
			u64 host_min_quantum = remaining <= 1000 ? 10 : 50;
#else
			// Host scheduler quantum for windows (worst case)
			// NOTE: On ps3 this function has very high accuracy
			constexpr u64 host_min_quantum = 500;
#endif
			if (remaining >= host_min_quantum)
			{
#ifdef __linux__
				// Do not wait for the last quantum to avoid loss of accuracy
				thread_ctrl::wait_for(remaining - ((remaining % host_min_quantum) + host_min_quantum), false);
#else
				// Wait on multiple of min quantum for large durations to avoid overloading low thread cpus
				thread_ctrl::wait_for(remaining - (remaining % host_min_quantum), false);
#endif
			}
			// TODO: Determine best value for yield delay
			else if (remaining >= host_min_quantum / 2)
			{
				std::this_thread::yield();
			}
			else
			{
				busy_wait(100);
			}

			const u64 current = rsx::uclock();

			if (current >= until)
			{
				break;
			}

			remaining = until - current;
		}
	}

	u32 thread::get_fifo_cmd() const
	{
		// Last fifo cmd for logging and utility
		return fifo_ctrl->last_cmd();
	}

	void invalid_method(thread*, u32, u32);

	std::string thread::dump_regs() const
	{
		std::string result;

		if (ctrl)
		{
			fmt::append(result, "FIFO: GET=0x%07x, PUT=0x%07x, REF=0x%08x\n", +ctrl->get, +ctrl->put, +ctrl->ref);
		}

		for (u32 i = 0; i < 1 << 14; i++)
		{
			if (rsx::methods[i] == &invalid_method)
			{
				continue;
			}

			switch (i)
			{
			case NV4097_NO_OPERATION:
			case NV4097_INVALIDATE_L2:
			case NV4097_INVALIDATE_VERTEX_FILE:
			case NV4097_INVALIDATE_VERTEX_CACHE_FILE:
			case NV4097_INVALIDATE_ZCULL:
			case NV4097_WAIT_FOR_IDLE:
			case NV4097_PM_TRIGGER:
			case NV4097_ZCULL_SYNC:
				continue;

			default:
			{
				if (i >= NV308A_COLOR && i < NV3089_SET_OBJECT)
				{
					continue;
				}

				break;
			}
			}

			fmt::append(result, "[%04x] %s\n", i, ensure(rsx::get_pretty_printing_function(i))(i, method_registers.registers[i]));
		}

		return result;
	}

	flags32_t thread::read_barrier(u32 memory_address, u32 memory_range, bool unconditional)
	{
		flags32_t zcull_flags = (unconditional)? reports::sync_none : reports::sync_defer_copy;
		return zcull_ctrl->read_barrier(this, memory_address, memory_range, zcull_flags);
	}

	void thread::notify_zcull_info_changed()
	{
		check_zcull_status(false);
	}

	void thread::on_notify_memory_mapped(u32 address, u32 size)
	{
		// In the case where an unmap is followed shortly after by a remap of the same address space
		// we must block until RSX has invalidated the memory
		// or lock m_mtx_task and do it ourselves

		if (m_rsx_thread_exiting)
			return;

		reader_lock lock(m_mtx_task);

		const auto map_range = address_range::start_length(address, size);

		if (!m_invalidated_memory_range.valid())
			return;

		if (m_invalidated_memory_range.overlaps(map_range))
		{
			lock.upgrade();
			handle_invalidated_memory_range();
		}
	}

	void thread::on_notify_memory_unmapped(u32 address, u32 size)
	{
		if (!m_rsx_thread_exiting && address < rsx::constants::local_mem_base)
		{
			if (!isHLE)
			{
				// Each bit represents io entry to be unmapped
				u64 unmap_status[512 / 64]{};

				for (u32 ea = address >> 20, end = ea + (size >> 20); ea < end; ea++)
				{
					const u32 io = utils::rol32(iomap_table.io[ea], 32 - 20);

					if (io + 1)
					{
						unmap_status[io / 64] |= 1ull << (io & 63);
						iomap_table.ea[io].release(-1);
						iomap_table.io[ea].release(-1);
					}
				}

				for (u32 i = 0; i < std::size(unmap_status); i++)
				{
					// TODO: Check order when sending multiple events
					if (u64 to_unmap = unmap_status[i])
					{
						// Each 64 entries are grouped by a bit
						const u64 io_event = SYS_RSX_EVENT_UNMAPPED_BASE << i;
						send_event(0, io_event, to_unmap);
					}
				}
			}
			else
			{
				// TODO: Fix this
				u32 ea = address >> 20, io = iomap_table.io[ea];

				if (io + 1)
				{
					io >>= 20;

					auto& cfg = g_fxo->get<gcm_config>();
					std::lock_guard lock(cfg.gcmio_mutex);

					for (const u32 end = ea + (size >> 20); ea < end;)
					{
						cfg.offsetTable.ioAddress[ea++] = 0xFFFF;
						cfg.offsetTable.eaAddress[io++] = 0xFFFF;
					}
				}
			}

			// Pause RSX thread momentarily to handle unmapping
			eng_lock elock(this);

			// Queue up memory invalidation
			std::lock_guard lock(m_mtx_task);
			const bool existing_range_valid = m_invalidated_memory_range.valid();
			const auto unmap_range = address_range::start_length(address, size);

			// Pause RSX thread momentarily to handle unmapping
			eng_lock elock(this);

			if (existing_range_valid && m_invalidated_memory_range.touches(unmap_range))
			{
				// Merge range-to-invalidate in case of consecutive unmaps
				m_invalidated_memory_range.set_min_max(unmap_range);
			}
			else
			{
				if (existing_range_valid)
				{
					// We can only delay consecutive unmaps.
					// Otherwise, to avoid VirtualProtect failures, we need to do the invalidation here
					handle_invalidated_memory_range();
				}

				m_invalidated_memory_range = unmap_range;
			}

			m_eng_interrupt_mask |= rsx::memory_config_interrupt;
		}
	}

	// NOTE: m_mtx_task lock must be acquired before calling this method
	void thread::handle_invalidated_memory_range()
	{
		m_eng_interrupt_mask.clear(rsx::memory_config_interrupt);

		if (!m_invalidated_memory_range.valid())
			return;

		on_invalidate_memory_range(m_invalidated_memory_range, rsx::invalidation_cause::unmap);
		m_invalidated_memory_range.invalidate();
	}

	//Pause/cont wrappers for FIFO ctrl. Never call this from rsx thread itself!
	void thread::pause()
	{
		external_interrupt_lock++;

		while (!external_interrupt_ack)
		{
			if (Emu.IsStopped())
				break;

			utils::pause();
		}
	}

	void thread::unpause()
	{
		// TODO: Clean this shit up
		external_interrupt_lock--;
	}

	void thread::wait_pause()
	{
		do
		{
			if (g_cfg.video.multithreaded_rsx)
			{
				g_fxo->get<rsx::dma_manager>().sync();
			}

			external_interrupt_ack.store(true);

			while (external_interrupt_lock)
			{
				// TODO: Investigate non busy-spinning method
				utils::pause();
			}

			external_interrupt_ack.store(false);
		}
		while (external_interrupt_lock);
	}

	u32 thread::get_load()
	{
		//Average load over around 30 frames
		if (!performance_counters.last_update_timestamp || performance_counters.sampled_frames > 30)
		{
			const auto timestamp = rsx::uclock();
			const auto idle = performance_counters.idle_time.load();
			const auto elapsed = timestamp - performance_counters.last_update_timestamp;

			if (elapsed > idle)
				performance_counters.approximate_load = static_cast<u32>((elapsed - idle) * 100 / elapsed);
			else
				performance_counters.approximate_load = 0u;

			performance_counters.idle_time = 0;
			performance_counters.sampled_frames = 0;
			performance_counters.last_update_timestamp = timestamp;
		}

		return performance_counters.approximate_load;
	}

	void thread::on_frame_end(u32 buffer, bool forced)
	{
		// Marks the end of a frame scope GPU-side
		if (g_user_asked_for_frame_capture.exchange(false) && !capture_current_frame)
		{
			capture_current_frame = true;
			frame_debug.reset();
			frame_capture.reset();

			// random number just to jumpstart the size
			frame_capture.replay_commands.reserve(8000);

			// capture first tile state with nop cmd
			rsx::frame_capture_data::replay_command replay_cmd;
			replay_cmd.rsx_command = std::make_pair(NV4097_NO_OPERATION, 0);
			frame_capture.replay_commands.push_back(replay_cmd);
			capture::capture_display_tile_state(this, frame_capture.replay_commands.back());
		}
		else if (capture_current_frame)
		{
			capture_current_frame = false;

			const std::string file_path = fs::get_config_dir() + "captures/" + Emu.GetTitleID() + "_" + date_time::current_time_narrow() + "_capture.rrc";

			// todo: may want to compress this data?
			utils::serial save_manager;
			save_manager.reserve(0x800'0000); // 128MB

			save_manager(frame_capture);

			fs::pending_file temp(file_path);

			if (temp.file && (temp.file.write(save_manager.data), temp.commit(false)))
			{
				rsx_log.success("Capture successful: %s", file_path);
			}
			else
			{
				rsx_log.fatal("Capture failed: %s (%s)", file_path, fs::g_tls_error);
			}

			frame_capture.reset();
			Emu.Pause();
		}

		if (zcull_ctrl->has_pending())
		{
			// NOTE: This is a workaround for buggy games.
			// Some applications leave the zpass/stats gathering active but don't use the information.
			// This can lead to the zcull unit using up all the memory queueing up operations that never get consumed.
			// Seen in Diablo III and Yakuza 5
			zcull_ctrl->clear(this, CELL_GCM_ZPASS_PIXEL_CNT | CELL_GCM_ZCULL_STATS);
		}

		// Save current state
		m_queued_flip.stats = m_frame_stats;
		m_queued_flip.push(buffer);
		m_queued_flip.skip_frame = skip_current_frame;

		if (!forced) [[likely]]
		{
			if (!g_cfg.video.disable_FIFO_reordering)
			{
				// Try to enable FIFO optimizations
				// Only rarely useful for some games like RE4
				m_flattener.evaluate_performance(m_frame_stats.draw_calls);
			}

			if (g_cfg.video.frame_skip_enabled)
			{
				m_skip_frame_ctr++;

				if (m_skip_frame_ctr >= g_cfg.video.consecutive_frames_to_draw)
					m_skip_frame_ctr = -g_cfg.video.consecutive_frames_to_skip;

				skip_current_frame = (m_skip_frame_ctr < 0);
			}
		}
		else
		{
			if (!g_cfg.video.disable_FIFO_reordering)
			{
				// Flattener is unusable due to forced random flips
				m_flattener.force_disable();
			}

			if (g_cfg.video.frame_skip_enabled)
			{
				rsx_log.error("Frame skip is not compatible with this application");
			}
		}

		// Reset current stats
		m_frame_stats = {};
		m_profiler.enabled = !!g_cfg.video.overlay;
	}

	void thread::request_emu_flip(u32 buffer)
	{
		if (is_current_thread()) // requested through command buffer
		{
			// NOTE: The flip will clear any queued flip requests
			handle_emu_flip(buffer);
		}
		else // requested 'manually' through ppu syscall
		{
			if (async_flip_requested & flip_request::emu_requested)
			{
				// ignore multiple requests until previous happens
				return;
			}

			async_flip_buffer = buffer;
			async_flip_requested |= flip_request::emu_requested;
			m_eng_interrupt_mask |= rsx::display_interrupt;
		}
	}

	void thread::handle_emu_flip(u32 buffer)
	{
		if (m_queued_flip.in_progress)
		{
			// Rescursion not allowed!
			return;
		}

		if (!m_queued_flip.pop(buffer))
		{
			// Frame was not queued before flipping
			on_frame_end(buffer, true);
			ensure(m_queued_flip.pop(buffer));
		}

		double limit = 0.;
		switch (g_disable_frame_limit ? frame_limit_type::none : g_cfg.video.frame_limit)
		{
		case frame_limit_type::none: limit = 0.; break;
		case frame_limit_type::_59_94: limit = 59.94; break;
		case frame_limit_type::_50: limit = 50.; break;
		case frame_limit_type::_60: limit = 60.; break;
		case frame_limit_type::_30: limit = 30.; break;
		case frame_limit_type::_auto: limit = static_cast<double>(g_cfg.video.vblank_rate); break;
		case frame_limit_type::_ps3: limit = 0.; break;
		default:
			break;
		}

		if (limit)
		{
			const u64 time = rsx::uclock() - Emu.GetPauseTime();
			const u64 needed_us = static_cast<u64>(1000000 / limit);

			if (int_flip_index == 0)
			{
				target_rsx_flip_time = time;
			}
			else
			{
				do
				{
					target_rsx_flip_time += needed_us;
				}
				while (time >= target_rsx_flip_time + needed_us);

				if (target_rsx_flip_time > time + 1000)
				{
					const auto delay_us = target_rsx_flip_time - time;
					lv2_obj::wait_timeout<false, false>(delay_us);

					if (thread_ctrl::state() == thread_state::aborting)
					{
						return;
					}

					performance_counters.idle_time += delay_us;
				}
			}
		}
		else if (wait_for_flip_sema)
		{
			const auto& value = vm::_ref<RsxSemaphore>(device_addr + 0x30).val;
			if (value != flip_sema_wait_val)
			{
				// Not yet signaled, handle it later
				async_flip_requested |= flip_request::emu_requested;
				async_flip_buffer = buffer;
				return;
			}

			wait_for_flip_sema = false;
		}

		int_flip_index++;

		current_display_buffer = buffer;
		m_queued_flip.emu_flip = true;
		m_queued_flip.in_progress = true;

		flip(m_queued_flip);

		last_guest_flip_timestamp = rsx::uclock() - 1000000;
		flip_status = CELL_GCM_DISPLAY_FLIP_STATUS_DONE;
		m_queued_flip.in_progress = false;

		if (!isHLE)
		{
			sys_rsx_context_attribute(0x55555555, 0xFEC, buffer, 0, 0, 0);
			return;
		}

		if (flip_handler)
		{
			intr_thread->cmd_list
			({
				{ ppu_cmd::set_args, 1 }, u64{ 1 },
				{ ppu_cmd::lle_call, flip_handler },
				{ ppu_cmd::sleep, 0 }
			});

			intr_thread->cmd_notify++;
			intr_thread->cmd_notify.notify_one();
		}
	}
}

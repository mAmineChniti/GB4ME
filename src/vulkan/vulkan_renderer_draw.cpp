// GB4ME Vulkan renderer — frame recording + present paths (plain, with-GUI, menu).
// See include/gb/vulkan_renderer.h for the class interface.

#include "gb/vulkan_renderer.h"
#include "vulkan/vulkan_internal.h"

#include <SDL3/SDL_vulkan.h>
#include <cmath>
#include <cstring>
#include <limits>

namespace gb
{
using namespace vulkan_detail;

void VulkanRenderer::record_command_buffer(VkCommandBuffer cmd, u32 image_index)
{
	VkCommandBufferBeginInfo begin{};
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	vkBeginCommandBuffer(cmd, &begin);

	// Upload: staging buffer -> texture image.
	const bool first_upload = (texture_layout_ == VK_IMAGE_LAYOUT_UNDEFINED);
	VkImageMemoryBarrier to_transfer{};
	to_transfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	to_transfer.oldLayout = texture_layout_;
	to_transfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	to_transfer.srcAccessMask = first_upload ? 0 : VK_ACCESS_SHADER_READ_BIT;
	to_transfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	to_transfer.image = texture_image;
	to_transfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	to_transfer.subresourceRange.levelCount = 1;
	to_transfer.subresourceRange.layerCount = 1;
	vkCmdPipelineBarrier(
		cmd,
		first_upload ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_transfer);
	VkBufferImageCopy region{};
	region.imageExtent = {tex_w_, tex_h_, 1};
	region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	region.imageSubresource.layerCount = 1;
	vkCmdCopyBufferToImage(cmd, texture_buffer, texture_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
						   1, &region);
	VkImageMemoryBarrier to_shader = to_transfer;
	to_shader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	to_shader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	to_shader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	to_shader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
						 0, 0, nullptr, 0, nullptr, 1, &to_shader);
	texture_layout_ = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

	VkClearValue clear{};
	clear.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
	VkRenderPassBeginInfo rp{};
	rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	rp.renderPass = render_pass;
	rp.framebuffer = swapchain_framebuffers[image_index];
	rp.renderArea.extent = swapchain_extent;
	rp.clearValueCount = 1;
	rp.pClearValues = &clear;
	vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
	// Letterbox the 10:9 GB image into the window: largest integer-friendly
	// fit centered, black bars elsewhere (bars show the clear color).
	{
		const float win_w = static_cast<float>(swapchain_extent.width);
		const float win_h = static_cast<float>(swapchain_extent.height);
		const PixelRect pr = fit_aspect(0.0f, 0.0f, win_w, win_h);
		VkViewport viewport{};
		viewport.x = pr.x;
		viewport.y = pr.y;
		viewport.width = pr.w;
		viewport.height = pr.h;
		viewport.minDepth = 0.0f;
		viewport.maxDepth = 1.0f;
		vkCmdSetViewport(cmd, 0, 1, &viewport);
		// Floor/ceil rather than truncate: a fractional viewport offset with a
		// truncated scissor clips the right/bottom edge of the image.
		VkRect2D scissor{};
		scissor.offset = {static_cast<i32>(std::floor(viewport.x)),
						  static_cast<i32>(std::floor(viewport.y))};
		scissor.extent = {static_cast<u32>(std::ceil(viewport.x + viewport.width)) -
							  static_cast<u32>(std::floor(viewport.x)),
						  static_cast<u32>(std::ceil(viewport.y + viewport.height)) -
							  static_cast<u32>(std::floor(viewport.y))};
		vkCmdSetScissor(cmd, 0, 1, &scissor);
	}
	push_video_constants(cmd);
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, graphics_pipeline);
	VkBuffer vbs[1] = {vertex_buffer};
	VkDeviceSize offs[1] = {0};
	vkCmdBindVertexBuffers(cmd, 0, 1, vbs, offs);
	vkCmdBindIndexBuffer(cmd, index_buffer, 0, VK_INDEX_TYPE_UINT16);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0, 1,
							&descriptor_sets[current_frame], 0, nullptr);
	vkCmdDrawIndexed(cmd, 6, 1, 0, 0, 0);
	draw_imgui(cmd);
	draw_text(cmd);
	vkCmdEndRenderPass(cmd);
	vkEndCommandBuffer(cmd);
}

void VulkanRenderer::render(const PPU::FrameData &frame)
{
	prepare_imgui();
	if (!ensure_texture(kTexWidth, kTexHeight))
		return;
	vkWaitForFences(device, 1, &in_flight_fences[current_frame], VK_TRUE,
					std::numeric_limits<u64>::max());
	u32 image_index = 0;
	VkResult acq = vkAcquireNextImageKHR(device, swapchain, std::numeric_limits<u64>::max(),
										 image_available_semaphores[current_frame], VK_NULL_HANDLE,
										 &image_index);
	if (acq == VK_ERROR_OUT_OF_DATE_KHR) {
		recreate_swapchain();
		return;
	}
	vkResetFences(device, 1, &in_flight_fences[current_frame]);

	// PPU framebuffer is 0xAARRGGBB, matching B8G8R8A8 byte order: direct copy.
	void *dst = nullptr;
	vkMapMemory(device, texture_buffer_memory, 0, static_cast<VkDeviceSize>(tex_w_) * tex_h_ * 4, 0,
				&dst);
	std::memcpy(dst, frame.pixels.data(), static_cast<size_t>(tex_w_) * tex_h_ * 4);
	vkUnmapMemory(device, texture_buffer_memory);

	update_uniform_buffer(current_frame);
	vkResetCommandBuffer(command_buffers[current_frame], 0);
	record_command_buffer(command_buffers[current_frame], image_index);
	present_frame(image_index);
}

void VulkanRenderer::render_gba(const u32 *pixels, u32 w, u32 h)
{
	prepare_imgui();
	if (pixels == nullptr || w == 0 || h == 0)
		return;
	if (!ensure_texture(w, h))
		return;
	vkWaitForFences(device, 1, &in_flight_fences[current_frame], VK_TRUE,
					std::numeric_limits<u64>::max());
	u32 image_index = 0;
	VkResult acq = vkAcquireNextImageKHR(device, swapchain, std::numeric_limits<u64>::max(),
										 image_available_semaphores[current_frame], VK_NULL_HANDLE,
										 &image_index);
	if (acq == VK_ERROR_OUT_OF_DATE_KHR) {
		recreate_swapchain();
		return;
	}
	vkResetFences(device, 1, &in_flight_fences[current_frame]);

	// GBA framebuffer is 0xAARRGGBB like the GB one: direct copy.
	void *dst = nullptr;
	vkMapMemory(device, texture_buffer_memory, 0, static_cast<VkDeviceSize>(tex_w_) * tex_h_ * 4, 0,
				&dst);
	std::memcpy(dst, pixels, static_cast<size_t>(tex_w_) * tex_h_ * 4);
	vkUnmapMemory(device, texture_buffer_memory);

	update_uniform_buffer(current_frame);
	vkResetCommandBuffer(command_buffers[current_frame], 0);
	record_command_buffer(command_buffers[current_frame], image_index);
	present_frame(image_index);
}

// Snap a draw rect to an exact integer multiple of the source size so every
// source pixel covers the same number of destination pixels. Returns the rect
// unchanged when integer scaling is off (Fit).
VulkanRenderer::PixelRect VulkanRenderer::fit_aspect(f32 x, f32 y, f32 w, f32 h) const
{
	// Pure geometry lives in types.h so the selftest can verify the aspect
	// property without a Vulkan device.
	const PixelRectF r = fit_aspect_rect(x, y, w, h, tex_w_, tex_h_);
	return {r.x, r.y, r.w, r.h};
}

void VulkanRenderer::push_video_state(VkCommandBuffer cmd, VideoFilter f, f32 grid)
{
	VideoPush p{};
	p.mode = static_cast<u32>(f);
	p.grid = grid;
	p.flags = 0;
	p.pad = 0;
	vkCmdPushConstants(cmd, pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(p), &p);
}

// Push the current upscaling filter + LCD grid depth for the next game draw.
void VulkanRenderer::push_video_constants(VkCommandBuffer cmd)
{
	push_video_state(cmd, video_filter_, lcd_grid_);
}

// The bezel photo is drawn with the SAME pipeline and fragment shader as the
// game, so it needs a neutral (Nearest, no grid) state: otherwise the LCD grid
// would be drawn over the entire shell photo, and the menu path (which draws
// the photo but no game) would read uninitialised push constants.
void VulkanRenderer::push_neutral_video_constants(VkCommandBuffer cmd)
{
	push_video_state(cmd, VideoFilter::Nearest, 0.0f);
}

void VulkanRenderer::present_frame(u32 image_index)
{
	VkSemaphore wait[1] = {image_available_semaphores[current_frame]};
	VkPipelineStageFlags stages[1] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
	VkSemaphore signal[1] = {render_finished_semaphores[current_frame]};
	VkSubmitInfo submit{};
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.waitSemaphoreCount = 1;
	submit.pWaitSemaphores = wait;
	submit.pWaitDstStageMask = stages;
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &command_buffers[current_frame];
	submit.signalSemaphoreCount = 1;
	submit.pSignalSemaphores = signal;
	vkQueueSubmit(graphics_queue, 1, &submit, in_flight_fences[current_frame]);

	VkPresentInfoKHR present{};
	present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	present.waitSemaphoreCount = 1;
	present.pWaitSemaphores = signal;
	present.swapchainCount = 1;
	present.pSwapchains = &swapchain;
	present.pImageIndices = &image_index;
	const VkResult res = vkQueuePresentKHR(present_queue, &present);
	if (res == VK_ERROR_OUT_OF_DATE_KHR || res == VK_SUBOPTIMAL_KHR) {
		recreate_swapchain();
	}
	current_frame = (current_frame + 1) % max_frames_in_flight;
}

void VulkanRenderer::record_command_buffer_with_gui(VkCommandBuffer cmd, u32 image_index,
													const GUIConsole &gui,
													const GUIConsole::Rect &game_rect)
{
	VkCommandBufferBeginInfo begin{};
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	vkBeginCommandBuffer(cmd, &begin);
	// Upload game texture as before
	const bool first_upload = (texture_layout_ == VK_IMAGE_LAYOUT_UNDEFINED);
	VkImageMemoryBarrier to_transfer{};
	to_transfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	to_transfer.oldLayout = texture_layout_;
	to_transfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	to_transfer.srcAccessMask = first_upload ? 0 : VK_ACCESS_SHADER_READ_BIT;
	to_transfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	to_transfer.image = texture_image;
	to_transfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	to_transfer.subresourceRange.levelCount = 1;
	to_transfer.subresourceRange.layerCount = 1;
	vkCmdPipelineBarrier(
		cmd,
		first_upload ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_transfer);
	VkBufferImageCopy region{};
	region.imageExtent = {tex_w_, tex_h_, 1};
	region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	region.imageSubresource.layerCount = 1;
	vkCmdCopyBufferToImage(cmd, texture_buffer, texture_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
						   1, &region);
	VkImageMemoryBarrier to_shader = to_transfer;
	to_shader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	to_shader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	to_shader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	to_shader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
						 0, 0, nullptr, 0, nullptr, 1, &to_shader);
	texture_layout_ = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	VkClearValue clear{}; // Body color as clear? Use dark gray, but GBC body will be drawn as rect,
						  // so clear to black is fine.
	clear.color = {{0.08f, 0.08f, 0.08f, 1.0f}};
	VkRenderPassBeginInfo rp{};
	rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	rp.renderPass = render_pass;
	rp.framebuffer = swapchain_framebuffers[image_index];
	rp.renderArea.extent = swapchain_extent;
	rp.clearValueCount = 1;
	rp.pClearValues = &clear;
	vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
	// Draw order (back to front): photo shell, game screen, GUI overlay.
	// (HiDPI: GUI coords are window pixels; scale to swapchain pixels.)
	int win_w = gui.window_w(), win_h = gui.window_h();
	const float scale_x = swapchain_extent.width / static_cast<float>(win_w);
	const float scale_y = swapchain_extent.height / static_cast<float>(win_h);
	// 1) Photo shell body (textured fullscreen quad clipped to body rect).
	if (body_photo_ok_ && !body_descriptor_sets.empty()) {
		auto bd = gui.body_rect();
		VkViewport vp_body{};
		vp_body.x = bd.x * scale_x;
		vp_body.y = bd.y * scale_y;
		vp_body.width = bd.w * scale_x;
		vp_body.height = bd.h * scale_y;
		vp_body.minDepth = 0;
		vp_body.maxDepth = 1;
		vkCmdSetViewport(cmd, 0, 1, &vp_body);
		VkRect2D sc_body{};
		sc_body.offset = {static_cast<i32>(vp_body.x), static_cast<i32>(vp_body.y)};
		sc_body.extent = {static_cast<u32>(vp_body.width), static_cast<u32>(vp_body.height)};
		vkCmdSetScissor(cmd, 0, 1, &sc_body);
		push_neutral_video_constants(cmd);
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, graphics_pipeline);
		VkBuffer vbs[1] = {vertex_buffer};
		VkDeviceSize offs[1] = {0};
		vkCmdBindVertexBuffers(cmd, 0, 1, vbs, offs);
		vkCmdBindIndexBuffer(cmd, index_buffer, 0, VK_INDEX_TYPE_UINT16);
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0, 1,
								&body_descriptor_sets[0], 0, nullptr);
		vkCmdDrawIndexed(cmd, 6, 1, 0, 0, 0);
	}
	// 2) Game texture inside the photo's LCD rect (caller passes the
	// aspect-correct rect: GB-shaped for GB/GBC, 3:2-fitted for GBA).
	const GUIConsole::Rect scr = game_rect;
	const PixelRect gp =
		fit_aspect(scr.x * scale_x, scr.y * scale_y, scr.w * scale_x, scr.h * scale_y);
	VkViewport vp_game{};
	vp_game.x = gp.x;
	vp_game.y = gp.y;
	vp_game.width = gp.w;
	vp_game.height = gp.h;
	vp_game.minDepth = 0;
	vp_game.maxDepth = 1;
	vkCmdSetViewport(cmd, 0, 1, &vp_game);
	VkRect2D sc_game{};
	sc_game.offset = {static_cast<i32>(std::floor(vp_game.x)),
					  static_cast<i32>(std::floor(vp_game.y))};
	sc_game.extent = {static_cast<u32>(std::ceil(vp_game.x + vp_game.width)) -
						  static_cast<u32>(std::floor(vp_game.x)),
					  static_cast<u32>(std::ceil(vp_game.y + vp_game.height)) -
						  static_cast<u32>(std::floor(vp_game.y))};
	vkCmdSetScissor(cmd, 0, 1, &sc_game);
	push_video_constants(cmd);
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, graphics_pipeline);
	VkBuffer vbs[1] = {vertex_buffer};
	VkDeviceSize offs[1] = {0};
	vkCmdBindVertexBuffers(cmd, 0, 1, vbs, offs);
	vkCmdBindIndexBuffer(cmd, index_buffer, 0, VK_INDEX_TYPE_UINT16);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0, 1,
							&descriptor_sets[current_frame], 0, nullptr);
	vkCmdDrawIndexed(cmd, 6, 1, 0, 0, 0);
	// 3) GUI overlay full-window: press highlights, gear, settings, modal.
	VkViewport vp_full{};
	vp_full.x = 0;
	vp_full.y = 0;
	vp_full.width = static_cast<float>(swapchain_extent.width);
	vp_full.height = static_cast<float>(swapchain_extent.height);
	vp_full.minDepth = 0;
	vp_full.maxDepth = 1;
	vkCmdSetViewport(cmd, 0, 1, &vp_full);
	VkRect2D sc_full{};
	sc_full.extent = swapchain_extent;
	vkCmdSetScissor(cmd, 0, 1, &sc_full);
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, gui_pipeline);
	VkBuffer gvbs[1] = {gui_vertex_buffer};
	VkDeviceSize goffs[1] = {0};
	vkCmdBindVertexBuffers(cmd, 0, 1, gvbs, goffs);
	vkCmdBindIndexBuffer(cmd, gui_index_buffer, 0, VK_INDEX_TYPE_UINT32);
	vkCmdDrawIndexed(cmd, gui_index_count, 1, 0, 0, 0);
	draw_imgui(cmd);
	draw_text(cmd);
	vkCmdEndRenderPass(cmd);
	vkEndCommandBuffer(cmd);
}

void VulkanRenderer::render_with_gui(const PPU::FrameData &frame, const GUIConsole &gui)
{
	prepare_imgui();
	// The game texture is shared with the GBA path (240x160). Without this,
	// a GB/GBC frame uploaded after any GBA frame lands in a 240-wide
	// staging image: row-pitch skew (diagonal distortion) plus a buffer
	// over-read past the 160x144 pixels. Every other upload path sizes the
	// texture first; this one must too.
	if (!ensure_texture(kTexWidth, kTexHeight))
		return;
	vkWaitForFences(device, 1, &in_flight_fences[current_frame], VK_TRUE,
					std::numeric_limits<u64>::max());
	u32 image_index = 0;
	VkResult acq = vkAcquireNextImageKHR(device, swapchain, std::numeric_limits<u64>::max(),
										 image_available_semaphores[current_frame], VK_NULL_HANDLE,
										 &image_index);
	if (acq == VK_ERROR_OUT_OF_DATE_KHR) {
		recreate_swapchain();
		return;
	}
	vkResetFences(device, 1, &in_flight_fences[current_frame]);
	void *dst = nullptr;
	vkMapMemory(device, texture_buffer_memory, 0, static_cast<VkDeviceSize>(tex_w_) * tex_h_ * 4, 0,
				&dst);
	std::memcpy(dst, frame.pixels.data(), static_cast<size_t>(tex_w_) * tex_h_ * 4);
	vkUnmapMemory(device, texture_buffer_memory);
	update_uniform_buffer(current_frame);
	const GUIConsole::Rect gb_rect = gui.screen_rect();
	update_gui_buffers(gui, &gb_rect);
	vkResetCommandBuffer(command_buffers[current_frame], 0);
	record_command_buffer_with_gui(command_buffers[current_frame], image_index, gui, gb_rect);
	VkSemaphore wait[1] = {image_available_semaphores[current_frame]};
	VkPipelineStageFlags stages[1] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
	VkSemaphore signal[1] = {render_finished_semaphores[current_frame]};
	VkSubmitInfo submit{};
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.waitSemaphoreCount = 1;
	submit.pWaitSemaphores = wait;
	submit.pWaitDstStageMask = stages;
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &command_buffers[current_frame];
	submit.signalSemaphoreCount = 1;
	submit.pSignalSemaphores = signal;
	vkQueueSubmit(graphics_queue, 1, &submit, in_flight_fences[current_frame]);
	VkPresentInfoKHR present{};
	present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	present.waitSemaphoreCount = 1;
	present.pWaitSemaphores = signal;
	present.swapchainCount = 1;
	present.pSwapchains = &swapchain;
	present.pImageIndices = &image_index;
	VkResult res = vkQueuePresentKHR(present_queue, &present);
	if (res == VK_ERROR_OUT_OF_DATE_KHR || res == VK_SUBOPTIMAL_KHR)
		recreate_swapchain();
	current_frame = (current_frame + 1) % max_frames_in_flight;
}

void VulkanRenderer::render_gba_with_gui(const u32 *pixels, u32 w, u32 h, const GUIConsole &gui)
{
	prepare_imgui();
	if (pixels == nullptr || w == 0 || h == 0)
		return;
	if (!ensure_texture(w, h))
		return;
	vkWaitForFences(device, 1, &in_flight_fences[current_frame], VK_TRUE,
					std::numeric_limits<u64>::max());
	u32 image_index = 0;
	VkResult acq = vkAcquireNextImageKHR(device, swapchain, std::numeric_limits<u64>::max(),
										 image_available_semaphores[current_frame], VK_NULL_HANDLE,
										 &image_index);
	if (acq == VK_ERROR_OUT_OF_DATE_KHR) {
		recreate_swapchain();
		return;
	}
	vkResetFences(device, 1, &in_flight_fences[current_frame]);

	// GBA framebuffer is 0xAARRGGBB like the GB one: direct copy.
	void *dst = nullptr;
	vkMapMemory(device, texture_buffer_memory, 0, static_cast<VkDeviceSize>(tex_w_) * tex_h_ * 4, 0,
				&dst);
	std::memcpy(dst, pixels, static_cast<size_t>(tex_w_) * tex_h_ * 4);
	vkUnmapMemory(device, texture_buffer_memory);

	update_uniform_buffer(current_frame);
	const GUIConsole::Rect gba_rect = gui.gba_screen_rect();
	update_gui_buffers(gui, &gba_rect);
	vkResetCommandBuffer(command_buffers[current_frame], 0);
	record_command_buffer_with_gui(command_buffers[current_frame], image_index, gui, gba_rect);
	VkSemaphore wait[1] = {image_available_semaphores[current_frame]};
	VkPipelineStageFlags stages[1] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
	VkSemaphore signal[1] = {render_finished_semaphores[current_frame]};
	VkSubmitInfo submit{};
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.waitSemaphoreCount = 1;
	submit.pWaitSemaphores = wait;
	submit.pWaitDstStageMask = stages;
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &command_buffers[current_frame];
	submit.signalSemaphoreCount = 1;
	submit.pSignalSemaphores = signal;
	vkQueueSubmit(graphics_queue, 1, &submit, in_flight_fences[current_frame]);
	VkPresentInfoKHR present{};
	present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	present.waitSemaphoreCount = 1;
	present.pWaitSemaphores = signal;
	present.swapchainCount = 1;
	present.pSwapchains = &swapchain;
	present.pImageIndices = &image_index;
	VkResult res = vkQueuePresentKHR(present_queue, &present);
	if (res == VK_ERROR_OUT_OF_DATE_KHR || res == VK_SUBOPTIMAL_KHR)
		recreate_swapchain();
	current_frame = (current_frame + 1) % max_frames_in_flight;
}

void VulkanRenderer::record_command_buffer_menu(VkCommandBuffer cmd, u32 image_index,
												const GUIConsole &gui)
{
	VkCommandBufferBeginInfo begin{};
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	vkBeginCommandBuffer(cmd, &begin);
	VkClearValue clear{};
	clear.color = {{0.08f, 0.08f, 0.08f, 1.0f}};
	VkRenderPassBeginInfo rp{};
	rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	rp.renderPass = render_pass;
	rp.framebuffer = swapchain_framebuffers[image_index];
	rp.renderArea.extent = swapchain_extent;
	rp.clearValueCount = 1;
	rp.pClearValues = &clear;
	vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
	// Draw order: photo shell, then GUI overlay (menu list is part of it).
	// (HiDPI: GUI coords are window pixels; scale to swapchain pixels.)
	int win_w = gui.window_w(), win_h = gui.window_h();
	const float scale_x = swapchain_extent.width / static_cast<float>(win_w);
	const float scale_y = swapchain_extent.height / static_cast<float>(win_h);
	// 1) Photo shell body (textured quad clipped to body rect).
	if (body_photo_ok_ && !body_descriptor_sets.empty()) {
		auto bd = gui.body_rect();
		VkViewport vp_body{};
		vp_body.x = bd.x * scale_x;
		vp_body.y = bd.y * scale_y;
		vp_body.width = bd.w * scale_x;
		vp_body.height = bd.h * scale_y;
		vp_body.minDepth = 0;
		vp_body.maxDepth = 1;
		vkCmdSetViewport(cmd, 0, 1, &vp_body);
		VkRect2D sc_body{};
		sc_body.offset = {static_cast<i32>(vp_body.x), static_cast<i32>(vp_body.y)};
		sc_body.extent = {static_cast<u32>(vp_body.width), static_cast<u32>(vp_body.height)};
		vkCmdSetScissor(cmd, 0, 1, &sc_body);
		push_neutral_video_constants(cmd);
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, graphics_pipeline);
		VkBuffer vbs[1] = {vertex_buffer};
		VkDeviceSize offs[1] = {0};
		vkCmdBindVertexBuffers(cmd, 0, 1, vbs, offs);
		vkCmdBindIndexBuffer(cmd, index_buffer, 0, VK_INDEX_TYPE_UINT16);
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0, 1,
								&body_descriptor_sets[0], 0, nullptr);
		vkCmdDrawIndexed(cmd, 6, 1, 0, 0, 0);
	}
	// 2) GUI overlay full-window: menu, buttons, settings, modal.
	VkViewport vp_full{};
	vp_full.x = 0;
	vp_full.y = 0;
	vp_full.width = static_cast<float>(swapchain_extent.width);
	vp_full.height = static_cast<float>(swapchain_extent.height);
	vp_full.minDepth = 0;
	vp_full.maxDepth = 1;
	vkCmdSetViewport(cmd, 0, 1, &vp_full);
	VkRect2D sc_full{};
	sc_full.extent = swapchain_extent;
	vkCmdSetScissor(cmd, 0, 1, &sc_full);
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, gui_pipeline);
	VkBuffer gvbs[1] = {gui_vertex_buffer};
	VkDeviceSize goffs[1] = {0};
	vkCmdBindVertexBuffers(cmd, 0, 1, gvbs, goffs);
	vkCmdBindIndexBuffer(cmd, gui_index_buffer, 0, VK_INDEX_TYPE_UINT32);
	vkCmdDrawIndexed(cmd, gui_index_count, 1, 0, 0, 0);
	draw_imgui(cmd);
	draw_text(cmd);
	vkCmdEndRenderPass(cmd);
	vkEndCommandBuffer(cmd);
}

void VulkanRenderer::render_menu(const GUIConsole &gui)
{
	prepare_imgui();
	vkWaitForFences(device, 1, &in_flight_fences[current_frame], VK_TRUE,
					std::numeric_limits<u64>::max());
	u32 image_index = 0;
	VkResult acq = vkAcquireNextImageKHR(device, swapchain, std::numeric_limits<u64>::max(),
										 image_available_semaphores[current_frame], VK_NULL_HANDLE,
										 &image_index);
	if (acq == VK_ERROR_OUT_OF_DATE_KHR) {
		recreate_swapchain();
		return;
	}
	vkResetFences(device, 1, &in_flight_fences[current_frame]);
	update_uniform_buffer(current_frame);
	update_gui_buffers(gui);
	vkResetCommandBuffer(command_buffers[current_frame], 0);
	record_command_buffer_menu(command_buffers[current_frame], image_index, gui);
	VkSemaphore wait[1] = {image_available_semaphores[current_frame]};
	VkPipelineStageFlags stages[1] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
	VkSemaphore signal[1] = {render_finished_semaphores[current_frame]};
	VkSubmitInfo submit{};
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.waitSemaphoreCount = 1;
	submit.pWaitSemaphores = wait;
	submit.pWaitDstStageMask = stages;
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &command_buffers[current_frame];
	submit.signalSemaphoreCount = 1;
	submit.pSignalSemaphores = signal;
	vkQueueSubmit(graphics_queue, 1, &submit, in_flight_fences[current_frame]);
	VkPresentInfoKHR present{};
	present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	present.waitSemaphoreCount = 1;
	present.pWaitSemaphores = signal;
	present.swapchainCount = 1;
	present.pSwapchains = &swapchain;
	present.pImageIndices = &image_index;
	VkResult res = vkQueuePresentKHR(present_queue, &present);
	if (res == VK_ERROR_OUT_OF_DATE_KHR || res == VK_SUBOPTIMAL_KHR)
		recreate_swapchain();
	current_frame = (current_frame + 1) % max_frames_in_flight;
}

} // namespace gb

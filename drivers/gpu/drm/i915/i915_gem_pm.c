/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2019 Intel Corporation
 */

#include "gt/intel_gt_pm.h"

#include "i915_drv.h"
#include "i915_gem_pm.h"
#include "i915_globals.h"

static void i915_gem_park(struct drm_i915_private *i915)
{
	struct intel_engine_cs *engine;
	enum intel_engine_id id;

	lockdep_assert_held(&i915->drm.struct_mutex);

	for_each_engine(engine, i915, id)
		i915_gem_batch_pool_fini(&engine->batch_pool);

	i915_timelines_park(i915);
	i915_vma_parked(i915);

	i915_globals_park();
}

static void idle_work_handler(struct work_struct *work)
{
	struct drm_i915_private *i915 =
		container_of(work, typeof(*i915), gem.idle_work.work);

	mutex_lock(&i915->drm.struct_mutex);

	intel_wakeref_lock(&i915->gt.wakeref);
	if (!intel_wakeref_active(&i915->gt.wakeref))
		i915_gem_park(i915);
	intel_wakeref_unlock(&i915->gt.wakeref);

	mutex_unlock(&i915->drm.struct_mutex);
}

static void retire_work_handler(struct work_struct *work)
{
	struct drm_i915_private *i915 =
		container_of(work, typeof(*i915), gem.retire_work.work);

	/* Come back later if the device is busy... */
	if (mutex_trylock(&i915->drm.struct_mutex)) {
		i915_retire_requests(i915);
		mutex_unlock(&i915->drm.struct_mutex);
	}

	if (intel_wakeref_active(&i915->gt.wakeref))
		queue_delayed_work(i915->wq,
				   &i915->gem.retire_work,
				   round_jiffies_up_relative(HZ));
}

static int pm_notifier(struct notifier_block *nb,
		       unsigned long action,
		       void *data)
{
	struct drm_i915_private *i915 =
		container_of(nb, typeof(*i915), gem.pm_notifier);

	switch (action) {
	case INTEL_GT_UNPARK:
		i915_globals_unpark();
		queue_delayed_work(i915->wq,
				   &i915->gem.retire_work,
				   round_jiffies_up_relative(HZ));
		break;

	case INTEL_GT_PARK:
		mod_delayed_work(i915->wq,
				 &i915->gem.idle_work,
				 msecs_to_jiffies(100));
		break;
	}

	return NOTIFY_OK;
}

static bool switch_to_kernel_context_sync(struct drm_i915_private *i915)
{
	bool result = true;

	do {
		if (i915_gem_wait_for_idle(i915,
					   I915_WAIT_LOCKED |
					   I915_WAIT_FOR_IDLE_BOOST,
					   I915_GEM_IDLE_TIMEOUT) == -ETIME) {
			/* XXX hide warning from gem_eio */
			if (i915_modparams.reset) {
				dev_err(i915->drm.dev,
					"Failed to idle engines, declaring wedged!\n");
				GEM_TRACE_DUMP();
			}

			/*
			 * Forcibly cancel outstanding work and leave
			 * the gpu quiet.
			 */
			i915_gem_set_wedged(i915);
			result = false;
		}
	} while (i915_retire_requests(i915) && result);

	GEM_BUG_ON(i915->gt.awake);
	return result;
}

bool i915_gem_load_power_context(struct drm_i915_private *i915)
{
	return switch_to_kernel_context_sync(i915);
}

void i915_gem_suspend(struct drm_i915_private *i915)
{
	GEM_TRACE("\n");

	flush_workqueue(i915->wq);

	mutex_lock(&i915->drm.struct_mutex);

	/*
	 * We have to flush all the executing contexts to main memory so
	 * that they can saved in the hibernation image. To ensure the last
	 * context image is coherent, we have to switch away from it. That
	 * leaves the i915->kernel_context still active when
	 * we actually suspend, and its image in memory may not match the GPU
	 * state. Fortunately, the kernel_context is disposable and we do
	 * not rely on its state.
	 */
	switch_to_kernel_context_sync(i915);

	mutex_unlock(&i915->drm.struct_mutex);

	/*
	 * Assert that we successfully flushed all the work and
	 * reset the GPU back to its idle, low power state.
	 */
	GEM_BUG_ON(i915->gt.awake);
	cancel_delayed_work_sync(&i915->gpu_error.hangcheck_work);

	drain_delayed_work(&i915->gem.retire_work);

	/*
	 * As the idle_work is rearming if it detects a race, play safe and
	 * repeat the flush until it is definitely idle.
	 */
	drain_delayed_work(&i915->gem.idle_work);

	i915_gem_drain_freed_objects(i915);

	intel_uc_suspend(i915);
}

void i915_gem_suspend_late(struct drm_i915_private *i915)
{
	struct drm_i915_gem_object *obj;
	struct list_head *phases[] = {
		&i915->mm.unbound_list,
		&i915->mm.bound_list,
		NULL
	}, **phase;

	/*
	 * Neither the BIOS, ourselves or any other kernel
	 * expects the system to be in execlists mode on startup,
	 * so we need to reset the GPU back to legacy mode. And the only
	 * known way to disable logical contexts is through a GPU reset.
	 *
	 * So in order to leave the system in a known default configuration,
	 * always reset the GPU upon unload and suspend. Afterwards we then
	 * clean up the GEM state tracking, flushing off the requests and
	 * leaving the system in a known idle state.
	 *
	 * Note that is of the upmost importance that the GPU is idle and
	 * all stray writes are flushed *before* we dismantle the backing
	 * storage for the pinned objects.
	 *
	 * However, since we are uncertain that resetting the GPU on older
	 * machines is a good idea, we don't - just in case it leaves the
	 * machine in an unusable condition.
	 */

	mutex_lock(&i915->drm.struct_mutex);
	for (phase = phases; *phase; phase++) {
		list_for_each_entry(obj, *phase, mm.link)
			WARN_ON(i915_gem_object_set_to_gtt_domain(obj, false));
	}
	mutex_unlock(&i915->drm.struct_mutex);

	intel_uc_sanitize(i915);
	i915_gem_sanitize(i915);
}

void i915_gem_resume(struct drm_i915_private *i915)
{
	GEM_TRACE("\n");

	WARN_ON(i915->gt.awake);

	mutex_lock(&i915->drm.struct_mutex);
	intel_uncore_forcewake_get(&i915->uncore, FORCEWAKE_ALL);

	i915_gem_restore_gtt_mappings(i915);
	i915_gem_restore_fences(i915);

	/*
	 * As we didn't flush the kernel context before suspend, we cannot
	 * guarantee that the context image is complete. So let's just reset
	 * it and start again.
	 */
	intel_gt_resume(i915);

	if (i915_gem_init_hw(i915))
		goto err_wedged;

	intel_uc_resume(i915);

	/* Always reload a context for powersaving. */
	if (!i915_gem_load_power_context(i915))
		goto err_wedged;

out_unlock:
	intel_uncore_forcewake_put(&i915->uncore, FORCEWAKE_ALL);
	mutex_unlock(&i915->drm.struct_mutex);
	return;

err_wedged:
	if (!i915_reset_failed(i915)) {
		dev_err(i915->drm.dev,
			"Failed to re-initialize GPU, declaring it wedged!\n");
		i915_gem_set_wedged(i915);
	}
	goto out_unlock;
}

void i915_gem_init__pm(struct drm_i915_private *i915)
{
	INIT_DELAYED_WORK(&i915->gem.idle_work, idle_work_handler);
	INIT_DELAYED_WORK(&i915->gem.retire_work, retire_work_handler);

	i915->gem.pm_notifier.notifier_call = pm_notifier;
	blocking_notifier_chain_register(&i915->gt.pm_notifications,
					 &i915->gem.pm_notifier);
}

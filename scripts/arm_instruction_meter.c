/* Host-only instruction measurements for the pinned Unicorn 2.1.4 checker.
 * Match the Python callback exactly: read SP before every ARM instruction. */
#include <stdint.h>
#include <stddef.h>

typedef int (*mesh_reg_read)(void *, int, void *);
struct mesh_arm_measurement {
  mesh_reg_read read_register;
  int sp_register, error;
  uint64_t minimum_sp, instructions;
};

size_t mesh_arm_measurement_size(void) { return sizeof(struct mesh_arm_measurement); }

void mesh_arm_measure_instruction(void *engine, uint64_t address,
                                  uint32_t size, void *user)
{
  struct mesh_arm_measurement *state = (struct mesh_arm_measurement *) user;
  uint32_t sp = 0;
  int error;
  (void) address;
  (void) size;
  error = state->read_register(engine, state->sp_register, &sp);
  if (error) state->error = error;
  if (sp < state->minimum_sp) state->minimum_sp = sp;
  ++state->instructions;
}

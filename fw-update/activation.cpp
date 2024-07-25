#include "fw-update/update_manager.hpp"
#include "fw-update/activation.hpp"

namespace pldm
{
namespace fw_update
{

void Delete::delete_()
{
  updateManager->clearActivationInfo();
}
}
}

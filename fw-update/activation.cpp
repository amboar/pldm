#include "fw-update/update_manager.hpp"
#include "fw-update/activation.hpp"

namespace pldm
{
namespace fw_update
{

Activations Activation::activation(Activations value)
{
    if (value == Activations::Activating)
    {
        deleteImpl.reset();
        updateManager->activatePackage();
    }
    else if (value == Activations::Active || value == Activations::Failed)
    {
        if (!deleteImpl)
        {
            deleteImpl = std::make_unique<Delete>(bus, objPath,
                                                  updateManager);
        }
    }

    return ActivationIntf::activation(value);
}

void Delete::delete_()
{
    updateManager->clearActivationInfo();
}
}
}

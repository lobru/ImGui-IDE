// Module boilerplate: registers the accessor as a "SourceCodeAccessor" modular
// feature so it appears in Editor Preferences > General > Source Code.

#include "ImGuiIDESourceCodeAccessor.h"

#include "Modules/ModuleManager.h"
#include "Features/IModularFeatures.h"

class FImGuiIDESourceCodeAccessModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		Accessor.RefreshAvailability();
		IModularFeatures::Get().RegisterModularFeature(TEXT("SourceCodeAccessor"), &Accessor);
	}

	virtual void ShutdownModule() override
	{
		IModularFeatures::Get().UnregisterModularFeature(TEXT("SourceCodeAccessor"), &Accessor);
	}

private:
	FImGuiIDESourceCodeAccessor Accessor;
};

IMPLEMENT_MODULE(FImGuiIDESourceCodeAccessModule, ImGuiIDESourceCodeAccess);

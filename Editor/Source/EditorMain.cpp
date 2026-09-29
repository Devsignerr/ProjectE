#include "Editor/EditorApplication.h"
#include "Core/Log.h"

int main()
{
	FLog::EnableHistory();
	FEditorApplication App;
	return App.Run();
}

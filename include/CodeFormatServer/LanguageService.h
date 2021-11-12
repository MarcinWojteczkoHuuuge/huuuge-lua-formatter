#pragma once

#include <memory>
#include <functional>
#include <map>
#include "CodeFormatServer/VSCode.h"

class LanguageService
{
public:
	using MessageHandle = std::function<std::shared_ptr<vscode::Serializable>(std::shared_ptr<vscode::Serializable>)>;

	LanguageService();
	~LanguageService();

	bool Initialize();

	std::shared_ptr<vscode::Serializable> Dispatch(std::string_view method,
	                                               std::shared_ptr<vscode::Serializable> param);

private:
	std::shared_ptr<vscode::InitializeResult> OnInitialize(std::shared_ptr<vscode::InitializeParams> param);

	std::shared_ptr<vscode::Serializable> OnDidChange(std::shared_ptr<vscode::DidChangeTextDocumentParams> param);

	// std::shared_ptr<JsonResponseProtocol> OnDocumentFormatting(std::shared_ptr<FormatRequestProtocol> request);

	std::map<std::string, MessageHandle, std::less<>> _handles;
};

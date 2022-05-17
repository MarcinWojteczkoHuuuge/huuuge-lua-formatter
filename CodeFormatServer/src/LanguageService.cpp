#include "CodeFormatServer/LanguageService.h"
#include <fstream>
#include <iostream>
#include <sstream>
#include "nlohmann/json.hpp"
#include "asio.hpp"
#include "CodeFormatServer/VSCode.h"
#include "CodeService/LuaCodeStyleOptions.h"
#include "CodeService/LuaFormatter.h"
#include "CodeFormatServer/LanguageClient.h"
#include "CodeFormatServer/Service/CodeFormatService.h"
#include "CodeFormatServer/Service/ModuleService.h"
#include "CodeFormatServer/Service/CompletionService.h"
#include "Util/Url.h"
#include "Util/format.h"
#include "LuaParser/LuaIdentify.h"

using namespace std::placeholders;

LanguageService::LanguageService()
{
}

LanguageService::~LanguageService()
{
}

bool LanguageService::Initialize()
{
	JsonProtocol("initialize", &LanguageService::OnInitialize);
	JsonProtocol("initialized", &LanguageService::OnInitialized);
	JsonProtocol("textDocument/didChange", &LanguageService::OnDidChange);
	JsonProtocol("textDocument/didOpen", &LanguageService::OnDidOpen);
	JsonProtocol("textDocument/didClose", &LanguageService::OnClose);
	JsonProtocol("config/editorconfig/update", &LanguageService::OnEditorConfigUpdate);
	JsonProtocol("config/moduleconfig/update", &LanguageService::OnModuleConfigUpdate);
	JsonProtocol("textDocument/formatting", &LanguageService::OnFormatting);
	JsonProtocol("textDocument/rangeFormatting", &LanguageService::OnRangeFormatting);
	JsonProtocol("textDocument/onTypeFormatting", &LanguageService::OnTypeFormatting);
	JsonProtocol("textDocument/codeAction", &LanguageService::OnCodeAction);
	JsonProtocol("workspace/executeCommand", &LanguageService::OnExecuteCommand);
	JsonProtocol("workspace/didChangeWatchedFiles", &LanguageService::OnDidChangeWatchedFiles);
	JsonProtocol("textDocument/completion", &LanguageService::OnCompletion);
	return true;
}

std::shared_ptr<vscode::Serializable> LanguageService::Dispatch(std::string_view method,
                                                                nlohmann::json params)
{
	auto it = _handles.find(method);
	if (it != _handles.end())
	{
		return it->second(params);
	}
	return nullptr;
}

std::shared_ptr<vscode::InitializeResult> LanguageService::OnInitialize(std::shared_ptr<vscode::InitializeParams> param)
{
	LanguageClient::GetInstance().InitializeService();

	auto result = std::make_shared<vscode::InitializeResult>();

	result->capabilities.documentFormattingProvider = true;
	result->capabilities.documentRangeFormattingProvider = true;

	vscode::DocumentOnTypeFormattingOptions typeOptions;

	typeOptions.firstTriggerCharacter = "\n";

	result->capabilities.documentOnTypeFormattingProvider = typeOptions;

	result->capabilities.textDocumentSync.change = vscode::TextDocumentSyncKind::Incremental;
	result->capabilities.textDocumentSync.openClose = true;

	LanguageClient::GetInstance().SetVscodeSettings(param->initializationOptions.vscodeConfig);

	result->capabilities.codeActionProvider = true;
	result->capabilities.executeCommandProvider.commands = {
		"emmylua.reformat.me",
		"emmylua.import.me",
		"emmylua.spell.correct"
	};

	result->capabilities.completionProvider.resolveProvider = false;
	result->capabilities.completionProvider.triggerCharacters = {};
	result->capabilities.completionProvider.completionItem.labelDetailsSupport = true;

	auto& editorConfigFiles = param->initializationOptions.editorConfigFiles;
	for (auto& configFile : editorConfigFiles)
	{
		LanguageClient::GetInstance().UpdateCodeStyleOptions(configFile.workspace, configFile.path);
	}

	auto& moduleConfigFiles = param->initializationOptions.moduleConfigFiles;
	for (auto& configFile : moduleConfigFiles)
	{
		LanguageClient::GetInstance().GetService<ModuleService>()->GetIndex().BuildModule(
			configFile.workspace, configFile.path);
	}

	LanguageClient::GetInstance().GetService<ModuleService>()->GetIndex().SetDefaultModule(param->rootPath);

	std::filesystem::path localePath = param->initializationOptions.localeRoot;
	localePath /= param->locale + ".json";

	if (std::filesystem::exists(localePath) && std::filesystem::is_regular_file(localePath))
	{
		std::fstream fin(localePath.string(), std::ios::in);

		if (fin.is_open())
		{
			std::stringstream s;
			s << fin.rdbuf();
			std::string jsonText = s.str();
			auto json = nlohmann::json::parse(jsonText);

			if (json.is_object())
			{
				std::map<std::string, std::string> languageMap;
				for (auto [key,value] : json.items())
				{
					languageMap.insert({key, value});
				}

				LanguageTranslator::GetInstance().SetLanguageMap(std::move(languageMap));
			}
		}
	}

	if (!param->initializationOptions.extensionChars.empty())
	{
		for (auto& c : param->initializationOptions.extensionChars)
		{
			LuaIdentify::AddIdentifyChar(c);
		}
	}

	LanguageClient::GetInstance().SetRoot(param->rootPath);
	auto dictionaryPath = param->initializationOptions.dictionaryPath;
	if (!dictionaryPath.empty())
	{
		LanguageClient::GetInstance().GetService<CodeFormatService>()->LoadDictionary(dictionaryPath);
	}
	return result;
}

std::shared_ptr<vscode::Serializable> LanguageService::OnInitialized(std::shared_ptr<vscode::Serializable> param)
{
	LanguageClient::GetInstance().UpdateModuleInfo();
	return nullptr;
}

std::shared_ptr<vscode::Serializable> LanguageService::OnDidChange(
	std::shared_ptr<vscode::DidChangeTextDocumentParams> param)
{
	if (param->contentChanges.size() == 1)
	{
		auto& content = param->contentChanges.front();
		if (content.range.has_value())
		{
			LanguageClient::GetInstance().UpdateFile(param->textDocument.uri, content.range.value(),
			                                         std::move(content.text));
		}
		else
		{
			LanguageClient::GetInstance().UpdateFile(param->textDocument.uri,
			                                         {vscode::Position(-1, 0), vscode::Position(-1, 0)},
			                                         std::move(content.text));
		}
	}
	else
	{
		LanguageClient::GetInstance().UpdateFile(param->textDocument.uri, param->contentChanges);
	}

	LanguageClient::GetInstance().DelayDiagnosticFile(param->textDocument.uri);
	return nullptr;
}

std::shared_ptr<vscode::Serializable> LanguageService::OnDidOpen(
	std::shared_ptr<vscode::DidOpenTextDocumentParams> param)
{
	LanguageClient::GetInstance().UpdateFile(param->textDocument.uri, {}, std::move(param->textDocument.text));
	LanguageClient::GetInstance().DiagnosticFile(param->textDocument.uri);
	return nullptr;
}

std::shared_ptr<vscode::Serializable> LanguageService::OnFormatting(
	std::shared_ptr<vscode::DocumentFormattingParams> param)
{
	auto parser = LanguageClient::GetInstance().GetFileParser(param->textDocument.uri);

	auto totalLine = parser->GetTotalLine();

	auto result = std::make_shared<vscode::DocumentFormattingResult>();

	if (totalLine == 0)
	{
		result->hasError = true;
		return result;
	}

	auto options = LanguageClient::GetInstance().GetOptions(param->textDocument.uri);

	if (parser->HasError())
	{
		result->hasError = true;
		return result;
	}

	auto newText = LanguageClient::GetInstance().GetService<CodeFormatService>()->Format(parser, options);

	auto& edit = result->edits.emplace_back();
	edit.newText = std::move(newText);
	edit.range = vscode::Range(
		vscode::Position(0, 0),
		vscode::Position(totalLine + 1, 0)
	);
	return result;
}

std::shared_ptr<vscode::Serializable> LanguageService::OnClose(
	std::shared_ptr<vscode::DidCloseTextDocumentParams> param)
{
	LanguageClient::GetInstance().ClearFile(param->textDocument.uri);

	return nullptr;
}

std::shared_ptr<vscode::Serializable> LanguageService::OnEditorConfigUpdate(
	std::shared_ptr<vscode::ConfigUpdateParams> param)
{
	switch (param->type)
	{
	case vscode::FileChangeType::Created:
	case vscode::FileChangeType::Changed:
		{
			LanguageClient::GetInstance().UpdateCodeStyleOptions(param->source.workspace, param->source.path);
			break;
		}
	case vscode::FileChangeType::Delete:
		{
			LanguageClient::GetInstance().RemoveCodeStyleOptions(param->source.workspace);
			break;
		}
	}

	LanguageClient::GetInstance().UpdateAllDiagnosis();

	return nullptr;
}


std::shared_ptr<vscode::Serializable> LanguageService::OnModuleConfigUpdate(
	std::shared_ptr<vscode::ConfigUpdateParams> param)
{
	switch (param->type)
	{
	case vscode::FileChangeType::Created:
	case vscode::FileChangeType::Changed:
		{
			LanguageClient::GetInstance().GetService<ModuleService>()
			                             ->GetIndex().BuildModule(param->source.workspace, param->source.path);
			break;
		}
	case vscode::FileChangeType::Delete:
		{
			LanguageClient::GetInstance().GetService<ModuleService>()
			                             ->GetIndex().RemoveModule(param->source.workspace);
			break;
		}
	}

	LanguageClient::GetInstance().UpdateModuleInfo();

	return nullptr;
}


std::shared_ptr<vscode::Serializable> LanguageService::OnRangeFormatting(
	std::shared_ptr<vscode::DocumentRangeFormattingParams> param)
{
	auto parser = LanguageClient::GetInstance().GetFileParser(param->textDocument.uri);

	auto result = std::make_shared<vscode::DocumentFormattingResult>();

	auto options = LanguageClient::GetInstance().GetOptions(param->textDocument.uri);

	if (parser->HasError())
	{
		result->hasError = true;
		return result;
	}

	LuaFormatRange formatRange(static_cast<int>(param->range.start.line), static_cast<int>(param->range.end.line));
	auto formatResult = LanguageClient::GetInstance().GetService<CodeFormatService>()->RangeFormat(
		formatRange, parser, options);

	auto& edit = result->edits.emplace_back();
	edit.newText = std::move(formatResult);
	edit.range = vscode::Range(
		vscode::Position(formatRange.StartLine, formatRange.StartCharacter),
		vscode::Position(formatRange.EndLine + 1, formatRange.EndCharacter)
	);
	return result;
}

std::shared_ptr<vscode::Serializable> LanguageService::OnTypeFormatting(
	std::shared_ptr<vscode::TextDocumentPositionParams> param)
{
	auto parser = LanguageClient::GetInstance().GetFileParser(param->textDocument.uri);
	auto position = param->position;

	auto result = std::make_shared<vscode::DocumentFormattingResult>();
	if (parser->IsEmptyLine(static_cast<int>(position.line) - 1))
	{
		result->hasError = true;
		return result;
	}

	auto options = LanguageClient::GetInstance().GetOptions(param->textDocument.uri);

	if (parser->HasError())
	{
		result->hasError = true;
		return result;
	}


	LuaFormatRange formattedRange(static_cast<int>(position.line) - 1, static_cast<int>(position.line) - 1);

	auto formatResult = LanguageClient::GetInstance().GetService<CodeFormatService>()->RangeFormat(
		formattedRange, parser, options);

	auto& edit = result->edits.emplace_back();
	edit.newText = std::move(formatResult);
	edit.range = vscode::Range(
		vscode::Position(formattedRange.StartLine, formattedRange.StartCharacter),
		vscode::Position(formattedRange.EndLine + 1, formattedRange.EndCharacter)
	);
	return result;
}

std::shared_ptr<vscode::CodeActionResult> LanguageService::OnCodeAction(std::shared_ptr<vscode::CodeActionParams> param)
{
	auto range = param->range;
	auto uri = param->textDocument.uri;
	auto filePath = url::UrlToFilePath(uri);
	auto codeActionResult = std::make_shared<vscode::CodeActionResult>();

	for (auto& diagnostic : param->context.diagnostics)
	{
		if (LanguageClient::GetInstance().GetService<CodeFormatService>()->IsCodeFormatDiagnostic(diagnostic))
		{
			auto& action = codeActionResult->actions.emplace_back();
			std::string title = "reformat";
			action.title = title;
			action.command.title = title;
			action.command.command = "emmylua.reformat.me";
			action.command.arguments.push_back(param->textDocument.uri);
			action.command.arguments.push_back(param->range.Serialize());

			action.kind = vscode::CodeActionKind::QuickFix;
		}
		else if (LanguageClient::GetInstance().GetService<CodeFormatService>()->IsSpellDiagnostic(diagnostic))
		{
			LanguageClient::GetInstance().GetService<CodeFormatService>()->MakeSpellActions(codeActionResult, diagnostic, param->textDocument.uri);
		}
		else if (LanguageClient::GetInstance().GetService<ModuleService>()->IsDiagnosticRange(filePath, range))
		{
			auto modules = LanguageClient::GetInstance().GetService<ModuleService>()->GetImportModules(filePath, range);
			auto& action = codeActionResult->actions.emplace_back();
			std::string title = "import me";
			action.title = title;
			action.command.title = title;
			action.command.command = "emmylua.import.me";
			action.command.arguments.push_back(param->textDocument.uri);
			action.command.arguments.push_back(param->range.Serialize());
			for (auto& module : modules)
			{
				auto object = nlohmann::json::object();
				object["moduleName"] = module.ModuleName;
				object["path"] = module.FilePath;
				object["name"] = module.Name;
				action.command.arguments.push_back(object);
			}

			action.kind = vscode::CodeActionKind::QuickFix;
		}
	}
	return codeActionResult;
}

std::shared_ptr<vscode::Serializable> LanguageService::OnExecuteCommand(
	std::shared_ptr<vscode::ExecuteCommandParams> param)
{
	auto result = std::make_shared<vscode::Serializable>();
	if (param->command == "emmylua.reformat.me")
	{
		if (param->arguments.size() < 2)
		{
			return result;
		}

		std::string uri = param->arguments[0];
		vscode::Range range;

		range.Deserialize(param->arguments[1]);

		auto parser = LanguageClient::GetInstance().GetFileParser(uri);

		auto applyParams = std::make_shared<vscode::ApplyWorkspaceEditParams>();

		auto options = LanguageClient::GetInstance().GetOptions(uri);

		if (parser->HasError())
		{
			return result;
		}

		auto it = applyParams->edit.changes.emplace(uri, std::vector<vscode::TextEdit>());
		auto& change = it.first->second;

		auto& edit = change.emplace_back();
		LuaFormatRange formattedRange(static_cast<int>(range.start.line), static_cast<int>(range.end.line));

		auto formatResult = LanguageClient::GetInstance().GetService<CodeFormatService>()->RangeFormat(
			formattedRange, parser, options);

		edit.newText = std::move(formatResult);

		edit.range = vscode::Range(
			vscode::Position(formattedRange.StartLine, formattedRange.StartCharacter),
			vscode::Position(formattedRange.EndLine + 1, formattedRange.EndCharacter)
		);

		LanguageClient::GetInstance().SendRequest("workspace/applyEdit", applyParams);
	}
	else if (param->command == "emmylua.import.me")
	{
		if (param->arguments.size() < 4)
		{
			return result;
		}

		std::string uri = param->arguments[0];
		std::string filePath = url::UrlToFilePath(uri);
		auto config = LanguageClient::GetInstance().GetService<ModuleService>()->GetIndex().GetConfig(filePath);
		if (!config)
		{
			return nullptr;
		}
		vscode::Range range;

		range.Deserialize(param->arguments[1]);

		std::string moduleName = param->arguments[2];

		std::string moduleDefineName = param->arguments[3];

		std::string requireString = Util::format("local {} = {}(\"{}\")\n", moduleDefineName, config->import_function,
		                                   moduleName);
		auto parser = LanguageClient::GetInstance().GetFileParser(uri);
		auto applyParams = std::make_shared<vscode::ApplyWorkspaceEditParams>();
		auto it = applyParams->edit.changes.emplace(uri, std::vector<vscode::TextEdit>());
		auto& change = it.first->second;

		auto& edit = change.emplace_back();

		edit.newText = requireString;

		edit.range = LanguageClient::GetInstance().GetService<ModuleService>()->FindRequireRange(parser, config);

		LanguageClient::GetInstance().SendRequest("workspace/applyEdit", applyParams);
	}
	else if(param->command == "emmylua.spell.correct")
	{
		std::string uri = param->arguments[0];
		vscode::Range range;

		range.Deserialize(param->arguments[1]);

		std::string newText = param->arguments[2];

		auto applyParams = std::make_shared<vscode::ApplyWorkspaceEditParams>();
		auto it = applyParams->edit.changes.emplace(uri, std::vector<vscode::TextEdit>());
		auto& change = it.first->second;

		auto& edit = change.emplace_back();

		edit.newText = newText;

		edit.range = range;

		LanguageClient::GetInstance().SendRequest("workspace/applyEdit", applyParams);
	}

	return result;
}

std::shared_ptr<vscode::Serializable> LanguageService::OnDidChangeWatchedFiles(
	std::shared_ptr<vscode::DidChangeWatchedFilesParams> param)
{
	for (auto& change : param->changes)
	{
		auto filePath = url::UrlToFilePath(change.uri);
		switch (change.type)
		{
		case vscode::FileChangeType::Created:
			{
				std::vector<std::string> files = {filePath};
				LanguageClient::GetInstance().GetService<ModuleService>()->GetIndex().BuildIndex(files);
				break;
			}
		case vscode::FileChangeType::Delete:
			{
				LanguageClient::GetInstance().GetService<ModuleService>()->GetIndex().ClearFile(filePath);
				break;
			}
		default:
			{
				break;
			}
		}
	}

	return nullptr;
}

std::shared_ptr<vscode::CompletionList> LanguageService::OnCompletion(std::shared_ptr<vscode::CompletionParams> param)
{
	auto list = std::make_shared<vscode::CompletionList>();

	if (!LanguageClient::GetInstance().GetSettings().autoImport)
	{
		return list;
	}

	auto uri = param->textDocument.uri;

	auto parser = LanguageClient::GetInstance().GetFileParser(uri);

	auto options = LanguageClient::GetInstance().GetOptions(uri);

	list->isIncomplete = true;
	list->items = LanguageClient::GetInstance().GetService<CompletionService>()->GetCompletions(
		param->textDocument.uri, param->position, parser, options);

	return list;
}

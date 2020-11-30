#include "ThreadedHasher.h"
#include "Window.h"
#include "FileData.h"
#include "components/AsyncNotificationComponent.h"
#include "LocaleES.h"
#include "guis/GuiMsgBox.h"
#include "Gamelist.h"
#include "RetroAchievements.h"

#include "SystemConf.h"
#include "SystemData.h"
#include "FileData.h"
#include <unordered_set>
#include <queue>
#include "ApiSystem.h"
#include "utils/StringUtil.h"

#define ICONINDEX _U("\uF002 ")

ThreadedHasher* ThreadedHasher::mInstance = nullptr;
bool ThreadedHasher::mPaused = false;

static std::mutex mLoaderLock;

ThreadedHasher::ThreadedHasher(Window* window, HasherType type, std::queue<FileData*> searchQueue, bool forceAllGames)
	: mWindow(window)
{
	mForce = forceAllGames;
	mExit = false;
	mType = type;

	mSearchQueue = searchQueue;
	mTotal = mSearchQueue.size();

	mWndNotification = mWindow->createAsyncNotificationComponent();

	if ((mType & HASH_CHEEVOS_MD5) == HASH_CHEEVOS_MD5)
		mCheevosHashes = RetroAchievements::getCheevosHashes();

	if (mType == HASH_CHEEVOS_MD5)
		mWndNotification->updateTitle(ICONINDEX + _("SEARCHING RETROACHIEVEMENTS"));
	else 
		mWndNotification->updateTitle(ICONINDEX + _("SEARCHING NETPLAY GAMES"));

	int num_threads = std::thread::hardware_concurrency() / 2;
	if (num_threads == 0)
		num_threads = 1;

	mThreadCount = num_threads;
	for (size_t i = 0; i < num_threads; i++)
		mThreads.push_back(new std::thread(&ThreadedHasher::run, this));
}

ThreadedHasher::~ThreadedHasher()
{
	mWndNotification->close();
	mWndNotification = nullptr;

	ThreadedHasher::mInstance = nullptr;
}

std::string ThreadedHasher::formatGameName(FileData* game)
{
	return "[" + game->getSystemName() + "] " + game->getName();
}

void ThreadedHasher::updateUI(FileData* fileData)
{
	std::string idx = std::to_string(mTotal + 1 - mSearchQueue.size()) + "/" + std::to_string(mTotal);
	int percent = 100 - (mSearchQueue.size() * 100 / mTotal);
		
	mWndNotification->updateText(formatGameName(fileData));
	mWndNotification->updatePercent(percent);	
}

void ThreadedHasher::run()
{
	std::unique_lock<std::mutex> lock(mLoaderLock);

	bool cheevos = ((mType & HASH_CHEEVOS_MD5) == HASH_CHEEVOS_MD5);
	bool netplay = ((mType & HASH_NETPLAY_CRC) == HASH_NETPLAY_CRC);

	while (!mExit && !mSearchQueue.empty())
	{
		FileData* game = mSearchQueue.front();
		updateUI(game);
		mSearchQueue.pop();

		lock.unlock();

		if (mPaused)
		{
			while (!mExit && mPaused)
			{
				std::this_thread::yield();
				std::this_thread::sleep_for(std::chrono::milliseconds(500));
			}
		}		

		if (netplay)
			game->checkCrc32(mForce);

		if (cheevos)
		{
			game->checkCheevosHash(mForce);

			if (mCheevosHashes.size() > 0)
			{
				auto cheevos = mCheevosHashes.find(Utils::String::toUpper(game->getMetadata(MetaDataId::CheevosHash)));
				if (cheevos != mCheevosHashes.cend())
					game->setMetadata(MetaDataId::CheevosId, cheevos->second);
			}
		}		

		lock.lock();
	}

	mThreadCount--;

	if (mThreadCount == 0)
	{
		lock.unlock();
		delete this;
		ThreadedHasher::mInstance = nullptr;
	}
}

void ThreadedHasher::start(Window* window, HasherType type, bool forceAllGames, bool silent)
{
	if (ThreadedHasher::mInstance != nullptr)
	{
		if (silent)
			return;

		window->pushGui(new GuiMsgBox(window, _("GAME HASHING IS RUNNING. DO YOU WANT TO STOP IT ?"), _("YES"), []
		{
			ThreadedHasher::stop();
		}, _("NO"), nullptr));

		return;
	}
	
	std::queue<FileData*> searchQueue;
	
	for (auto sys : SystemData::sSystemVector)
	{
		bool takeNetplay = (type & HASH_NETPLAY_CRC) && sys->isNetplaySupported();
		bool takeCheevos = (type & HASH_CHEEVOS_MD5) && sys->isCheevosSupported();

		if (!takeNetplay && !takeCheevos)
			continue;

		for (auto file : sys->getRootFolder()->getFilesRecursive(GAME))
		{
			bool netPlay = takeNetplay && (forceAllGames || file->getMetadata(MetaDataId::Crc32).empty());
			bool cheevos = takeCheevos && (forceAllGames || file->getMetadata(MetaDataId::CheevosHash).empty());

			if (netPlay || cheevos)
				searchQueue.push(file);
		}
	}

	if (searchQueue.size() == 0)
	{
		if (!silent)
			window->pushGui(new GuiMsgBox(window, _("NO GAMES FIT THAT CRITERIA.")));

		return;
	}

	ThreadedHasher::mInstance = new ThreadedHasher(window, type, searchQueue, forceAllGames);
}

void ThreadedHasher::stop()
{
	auto thread = ThreadedHasher::mInstance;
	if (thread == nullptr)
		return;

	try
	{
		thread->mExit = true;
	}
	catch (...) {}
}


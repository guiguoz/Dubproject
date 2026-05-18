#include "MainComponent.h"

#if JUCE_WINDOWS
 #include <windows.h>
 #include <DbgHelp.h>
#endif

//==============================================================================
/**
 * SaxFXLiveApplication — point d'entrée de l'application JUCE
 */
class SaxFXLiveApplication : public juce::JUCEApplication
{
public:
    SaxFXLiveApplication() = default;

    const juce::String getApplicationName() override
    {
        return JUCE_APPLICATION_NAME_STRING;
    }

    const juce::String getApplicationVersion() override
    {
        return JUCE_APPLICATION_VERSION_STRING;
    }

    bool moreThanOneInstanceAllowed() override { return false; }

    //==========================================================================
    void initialise(const juce::String& /*commandLine*/) override
    {
#if JUCE_WINDOWS
        SetUnhandledExceptionFilter([](EXCEPTION_POINTERS* ep) -> LONG
        {
            const auto dir = juce::File::getSpecialLocation(
                juce::File::userApplicationDataDirectory).getChildFile("DubEngine");
            dir.createDirectory();
            const auto ts  = juce::Time::getCurrentTime().formatted("%Y%m%d_%H%M%S");
            const auto dmp = dir.getChildFile("crash_" + ts + ".dmp");
            HANDLE hFile   = CreateFileW(dmp.getFullPathName().toWideCharPointer(),
                GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
            if (hFile != INVALID_HANDLE_VALUE)
            {
                MINIDUMP_EXCEPTION_INFORMATION mei { GetCurrentThreadId(), ep, FALSE };
                MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(),
                    hFile, MiniDumpNormal, ep ? &mei : nullptr, nullptr, nullptr);
                CloseHandle(hFile);
            }
            return EXCEPTION_CONTINUE_SEARCH;
        });
#endif
        mainWindow = std::make_unique<MainWindow>(getApplicationName());
    }

    void shutdown() override
    {
        mainWindow = nullptr;
    }

    void systemRequestedQuit() override
    {
        quit();
    }

    void anotherInstanceStarted(const juce::String& /*commandLine*/) override {}

    //==========================================================================
    /**
     * MainWindow — fenêtre principale de l'application
     */
    class MainWindow : public juce::DocumentWindow
    {
    public:
        explicit MainWindow(const juce::String& name)
            : juce::DocumentWindow(
                  name,
                  juce::Desktop::getInstance().getDefaultLookAndFeel()
                      .findColour(juce::ResizableWindow::backgroundColourId),
                  juce::DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar(true);
            setContentOwned(new MainComponent(), true);

            setResizable(true, true);
            setResizeLimits(640, 400, 1920, 1080);
            centreWithSize(getWidth(), getHeight());

            setVisible(true);
        }

        void closeButtonPressed() override
        {
            juce::JUCEApplication::getInstance()->systemRequestedQuit();
        }

        // Escape quitte le plein écran — remonte ici si aucun enfant ne consomme la touche.
        bool keyPressed(const juce::KeyPress& key) override
        {
            if (key == juce::KeyPress::escapeKey)
            {
                if (auto* peer = getPeer(); peer != nullptr && peer->isFullScreen())
                {
                    peer->setFullScreen(false);
                    return true;
                }
            }
            if (key == juce::KeyPress::spaceKey)
            {
                if (auto* mc = dynamic_cast<MainComponent*>(getContentComponent()))
                {
                    mc->triggerPanic();
                    return true;
                }
            }
            return DocumentWindow::keyPressed(key);
        }

    private:
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainWindow)
    };

private:
    std::unique_ptr<MainWindow> mainWindow;
};

//==============================================================================
// Macro JUCE — définit le point d'entrée main()
START_JUCE_APPLICATION(SaxFXLiveApplication)

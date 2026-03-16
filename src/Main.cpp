#include "MainComponent.h"

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

    private:
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainWindow)
    };

private:
    std::unique_ptr<MainWindow> mainWindow;
};

//==============================================================================
// Macro JUCE — définit le point d'entrée main()
START_JUCE_APPLICATION(SaxFXLiveApplication)

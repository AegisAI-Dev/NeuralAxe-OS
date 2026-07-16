module.exports = function (config) {
  config.set({
    basePath: '',
    frameworks: ['jasmine', '@angular-devkit/build-angular'],
    plugins: [
      require('karma-jasmine'),
      require('karma-chrome-launcher'),
      require('karma-jasmine-html-reporter'),
      require('karma-coverage'),
      require('karma-junit-reporter'),
      require('@angular-devkit/build-angular/plugins/karma')
    ],
    client: {
      jasmine: {
        // you can add configuration options for Jasmine here
      },
    },
    jasmineHtmlReporter: {
      suppressAll: true // removes the duplicated traces
    },
    coverageReporter: {
      dir: require('path').join(__dirname, './coverage/axe-os'),
      subdir: '.',
      reporters: [
        { type: 'html' },
        { type: 'text-summary' }
      ]
    },
    reporters: ['progress', 'kjhtml', 'junit'],
    junitReporter: {
      outputDir: '.',
      outputFile: 'report.xml',
      useBrowserName: false
    },
    port: 9876,
    // Use HTTP long-polling instead of websockets for the karma<->browser channel.
    // On Windows, Chromium/Edge teardown after single-run completion aborts the
    // websocket with a TCP RST, which karma-server sees as an uncaught
    // 'read ECONNRESET' AFTER all results are in and then exits non-zero despite
    // a fully green run. Polling sockets close gracefully, so real test failures
    // still produce their normal non-zero exit while green runs exit 0 reliably.
    transports: ['polling'],
    colors: true,
    logLevel: config.LOG_INFO,
    autoWatch: true,
    browsers: ['Chrome'],
    customLaunchers: {
      ChromeHeadlessCI: {
        base: 'ChromeHeadless',
        flags: ['--no-sandbox', '--disable-gpu']
      }
    },
    singleRun: false,
    restartOnFileChange: true
  });
};

// This is a basic Flutter integration test.
//
// Since integration tests run in a full Flutter application, they can interact
// with the host side of a plugin implementation, unlike Dart unit tests.
//
// For more information about Flutter integration tests, please see
// https://flutter.dev/to/integration-testing

import 'package:flutter_test/flutter_test.dart';
import 'package:integration_test/integration_test.dart';

import 'package:omni_ble/omni_ble.dart';

void main() {
  IntegrationTestWidgetsFlutterBinding.ensureInitialized();

  testWidgets('getCapabilities returns runtime info', (
    WidgetTester tester,
  ) async {
    const plugin = OmniBle();
    final capabilities = await plugin.getCapabilities();

    expect(capabilities.platform.isNotEmpty, true);
    expect(capabilities.platformVersion.isNotEmpty, true);
  });
}

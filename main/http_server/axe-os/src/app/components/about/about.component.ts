import { Component } from '@angular/core';
import { NEURALAXE } from 'src/app/neuralaxe';

/**
 * About page: NeuralAxe OS product identity, upstream ESP-Miner / AxeOS
 * attribution (GPL-3.0) and the Bitcoin whitepaper. Static content only —
 * no telemetry, no network requests.
 */
@Component({
  selector: 'app-about',
  templateUrl: './about.component.html',
})
export class AboutComponent {
  public readonly neuralaxe = NEURALAXE;
}

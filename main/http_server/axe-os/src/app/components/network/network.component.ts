import { Component, ViewChild, AfterViewInit } from '@angular/core';
import { FormGroup } from '@angular/forms';
import { Observable, asyncScheduler } from 'rxjs';
import { observeOn } from 'rxjs/operators';
import { NetworkEditComponent } from '../network-edit/network.edit.component';

@Component({
  selector: 'app-network',
  templateUrl: './network.component.html',
  styleUrls: ['./network.component.scss'],
})
export class NetworkComponent implements AfterViewInit {
  form$!: Observable<FormGroup | null>;

  @ViewChild(NetworkEditComponent) networkEditComponent!: NetworkEditComponent;

  constructor() {}

  ngAfterViewInit() {
    // observeOn(asyncScheduler): form$ is only available after view init, so
    // emissions are deferred one tick to avoid NG0100 (value changing within
    // the same change-detection cycle). Display timing only.
    this.form$ = this.networkEditComponent.form$.pipe(observeOn(asyncScheduler));
  }
}
